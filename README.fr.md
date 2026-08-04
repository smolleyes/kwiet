<div align="center">

<img src="assets/svg/lockup.svg" alt="Kwiet" width="360">

**Votre micro, nettoyé par IA, dans toutes vos applications.**
Pas de micro virtuel. Pas de périphérique à changer. Un interrupteur.

[![CI](https://github.com/smolleyes/kwiet/actions/workflows/ci.yml/badge.svg)](https://github.com/smolleyes/kwiet/actions/workflows/ci.yml)
[![Licence](https://img.shields.io/badge/licence-Apache--2.0-9FD3C0)](LICENSE)
[![Windows 11](https://img.shields.io/badge/Windows-11-0D1417)](#prérequis)

[English](README.md) · [Architecture](docs/architecture.md)

</div>

---

## Le problème

Les outils de suppression de bruit installent un **micro virtuel**. Il faut donc
aller le sélectionner dans Teams, dans Discord, dans Meet, dans chaque
application — et recommencer chaque fois qu'une mise à jour remet le
périphérique par défaut. Quand ça tourne mal, l'application capte le mauvais
micro et personne ne vous entend.

Kwiet ne crée aucun périphérique. Il se greffe sur **votre** micro, à
l'intérieur du moteur audio de Windows. Les applications ne voient rien
d'inhabituel : elles ouvrent le micro qu'elles ont toujours ouvert, et le signal
qui en sort est déjà propre.

## Comment ça marche

Kwiet est un **APO** (*Audio Processing Object*) : une DLL COM en espace
utilisateur, chargée par `audiodg.exe`, le moteur audio de Windows. Aucun
pilote noyau.

Le débruitage est fait par [DeepFilterNet3](https://github.com/Rikorose/DeepFilterNet),
un réseau de ~2 M de paramètres — bien trop lourd pour le thread temps réel
d'`audiodg`, qui doit rendre un bloc toutes les 10 ms sans jamais allouer, ni
prendre un verrou, ni faire un appel système.

```
APOProcess()  — thread temps réel d'audiodg ————————————————————————
   |  pousse l'entrée dans un anneau SPSC sans verrou (préalloué)
   |  récupère la sortie traitée (retard fixe de 30 ms)
   +— si le worker est mort ou en retard : PASSTHROUGH immédiat
                                           jamais de silence, jamais de blocage
Worker  — thread de priorité normale ————————————————————————————————
   +— consomme l'anneau, appelle le cdylib Rust (DeepFilterNet3)
```

Le **fail-open est structurel** : un APO qui plante `audiodg` prive la machine
entière de son. Tout le design part de là. Si le DSP disparaît, le son passe
sans traitement — c'est le pire qui puisse arriver.

Détails et décisions : [`docs/architecture.md`](docs/architecture.md).

## Mesures

Sur Windows 11 build 26200, micro USB, 48 kHz stéréo :

| | |
|---|---|
| Suppression de bruit | **≥ 24 dB** sur source contrôlée |
| Latence ajoutée | **30 ms**, fixe |
| Charge CPU | **~2 %** d'un cœur (RTF 0,017–0,020) |
| Décrochages | 0 |

## Installation

> [!IMPORTANT]
> **Après l'installation, il reste une étape que Kwiet ne peut pas faire à votre
> place.** Windows 11 exige que vous choisissiez vous-même le pack d'effets :
> **Paramètres → Son → votre micro → Améliorations audio → Kwiet**.
> Tant que ce n'est pas fait, Kwiet est installé et complètement inerte. Le
> panneau vous le dira, avec un bouton pour ouvrir la bonne page.

1. Téléchargez le MSI correspondant à votre langue —
   `Kwiet_x.y.z_x64_fr-FR.msi` ou `Kwiet_x.y.z_x64_en-US.msi` — depuis les
   [releases](https://github.com/smolleyes/kwiet/releases).
2. Lancez-le. Il demande l'élévation : le pack d'effets est un package pilote,
   il s'enregistre auprès de Windows via `pnputil`. Le son est brièvement coupé.
3. Choisissez Kwiet dans les améliorations audio de votre micro (voir ci-dessus).
4. L'icône dans la zone de notification ouvre le panneau.

La désinstallation retire le pack du magasin de pilotes en même temps que
l'application.

### Prérequis

- Windows 11 **24H2 ou plus récent** (x64). Le mécanisme de pack d'effets
  utilisé ici n'existe pas sur les versions antérieures.
- Un micro qui négocie **48 kHz**. En dehors, Kwiet se met volontairement en
  passthrough plutôt que de rééchantillonner à l'aveugle.

## Le panneau

Une seule idée, répétée partout : **le céladon est ce que vos applications
reçoivent, l'ambre est ce que Kwiet a retiré.** L'écart entre les deux est le
produit.

- **Oscilloscope** — les dernières secondes, en deux enveloppes superposées.
- **VU-mètre** — l'instant, en une barre : le remplissage vif est le signal
  transmis, l'ambre qui dépasse est le bruit supprimé.
- **Intensité** — de discrète à maximale. Plus haut, le silence entre les mots
  devient total, au risque de raboter les attaques.
- **Interrupteur** — contournement immédiat, sans couper le flux.

Le panneau parle **français ou anglais**, selon Windows, et se bascule à la main
en bas à droite.

## Construire depuis les sources

Prérequis : Visual Studio 2019+ (charge de travail C++, SDK Windows 10/11),
CMake ≥ 3.21, Rust stable, Node 20+.

```powershell
# 1. L'APO (C++)
cmake -S apo -B apo/build -A x64
cmake --build apo/build --config Release

# 2. Le DSP (Rust, embarque le modèle DeepFilterNet3)
cd dsp ; cargo build --release ; cd ..

# 3. Le pack d'effets, signé et horodaté
.\installer\effectpack\build-package.ps1 -Version 0.2.2 `
    -CertPath mon-certificat.pfx -CertPassword $env:PFX_PW

# 4. L'application et l'installeur
cd ui ; npm ci ; npm run tauri build
# -> ui/src-tauri/target/release/bundle/msi/Kwiet_0.2.2_x64_fr-FR.msi
#    ui/src-tauri/target/release/bundle/msi/Kwiet_0.2.2_x64_en-US.msi
```

Le format est **MSI** et non NSIS : les stubs NSIS déclenchent régulièrement des
faux positifs antivirus, un paquet Windows Installer beaucoup moins. Le pack
d'effets est posé par une CustomAction différée, définie dans
[`ui/src-tauri/wix/effectpack.wxs`](ui/src-tauri/wix/effectpack.wxs).

Les icônes et le logo sont générés : `node assets/build-assets.mjs`.

Un APO en cours de développement se teste sur une VM ou une machine dédiée —
celui qui plante emporte le son de toute la machine :
[`docs/procedure-test-vm.md`](docs/procedure-test-vm.md).

## Structure du dépôt

| Dossier | Rôle |
|---|---|
| [`apo/`](apo/) | C++ : le shim COM, les anneaux SPSC, le worker, la mémoire partagée |
| [`dsp/`](dsp/) | Rust cdylib : DeepFilterNet3 derrière une ABI C stable |
| [`ui/`](ui/) | Tauri v2 : panneau, zone de notification, installeur MSI |
| [`installer/`](installer/) | Pack d'effets (INF, catalogue) et scripts d'installation |
| [`assets/`](assets/) | Identité visuelle, générée par script |
| [`bench/`](bench/) | Outils de mesure |
| [`docs/`](docs/) | Décisions d'architecture, procédure de test |

## Licence

[Apache-2.0](LICENSE).

DeepFilterNet3 est sous double licence MIT/Apache-2.0, compatible. Le modèle
embarqué provient du projet [DeepFilterNet](https://github.com/Rikorose/DeepFilterNet)
de Hendrik Schröter.
