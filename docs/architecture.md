# Architecture Kwiet — décisions

Document de référence. Les décisions marquées ✅ sont actées ; ne pas les
rouvrir sans élément nouveau.

## 1. Approche générale ✅

APO (Audio Processing Object) usermode, **pas de driver kernel**. DLL COM
chargée par `audiodg.exe`, enregistrée sur un endpoint de capture via le
registre — même mécanisme qu'Equalizer APO. Aucune signature de driver ;
code signing usermode plus tard (SignPath).

Références : samples `SwapAPO`/`SysVAD` du repo
[Windows-driver-samples](https://github.com/microsoft/Windows-driver-samples)
pour les interfaces et propriétés, source
d'[Equalizer APO](https://sourceforge.net/projects/equalizerapo/) pour les cas
tordus d'enregistrement sur capture.

## 2. Slot d'enregistrement : MFX ✅

L'APO est enregistré comme **effet de mode** (`PKEY_FX_ModeEffectClsid`,
valeur FxProperties `{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},6`).

Pourquoi MFX plutôt que :

- **SFX** (par flux, `,5`) : une instance par stream applicatif — Chrome +
  Discord = 2 × DFN3 en parallèle au jalon 2. Coût CPU inutile.
- **EFX** (endpoint, `,7`) : une seule instance et traite aussi les flux RAW,
  mais peu de précédents open source sur capture, quirks driver possibles.
- **LFX** (`,1`, modèle XP/7 déprécié) : chemin historique d'Equalizer APO —
  conservé comme **option de repli** si MFX pose problème sur un driver donné
  (install.ps1 accepte `-Slot SFX|EFX` ; LFX serait à ajouter au besoin).

Avec MFX : une instance par **mode de traitement actif** de l'endpoint,
partagée par toutes les applis du même mode, et un format stable côté
périphérique (l'engine resample côté flux, en aval). Coût DFN3 borné à
1-2 instances.

## 3. Modes de traitement déclarés ✅

`PKEY_MFX_ProcessingModes_Supported_For_Streaming`
(`{D3993A3F-99C2-4402-B5EC-A92A0367664B},6`, REG_MULTI_SZ) :

- `{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}` — DEFAULT
- `{98951333-B9CD-48B1-A0A3-FF40682D73F7}` — COMMUNICATIONS

COMMUNICATIONS est indispensable : Meet/Discord ouvrent leurs flux micro en
catégorie communications ; sans ce mode déclaré, l'APO ne serait jamais
instancié pour eux. Les flux RAW et exclusifs bypassent l'APO : accepté.

## 4. Chemin temps-réel ✅

`APOProcess()` tourne dans le thread RT d'audiodg (période 10 ms, 480 frames
@ 48 kHz float32). Règles absolues dans ce chemin :

- pas d'allocation, pas de lock, pas de syscall, pas de logging, pas de COM ;
- pas de page fault évitable (tout état préalloué à `LockForProcess`) ;
- **fail-open** : au moindre doute (worker mort, underrun, état incohérent),
  copie input→output. Jamais de silence produit par un bug, jamais de blocage.

Le DSP (DFN3, ~2M params, lookahead 2 frames) tourne dans un **worker thread**
priorité normale, dans le processus audiodg via la DLL :

```
APOProcess (RT)  → push ring SPSC input  → pop ring output (retard fixe 40-60 ms)
Worker           → consomme input (fenêtres 20 ms / hop 10 ms) → cdylib Rust → ring output
```

Les rings sont préalloués à `LockForProcess()` (contexte non-RT), le worker y
est démarré et arrêté à `UnlockForProcess()`.

## 5. ABI C entre apo/ et dsp/ ✅

```c
typedef struct KwietDsp KwietDsp;
KwietDsp* kwiet_dsp_create(uint32_t sample_rate, uint32_t channels); // hors RT
void      kwiet_dsp_destroy(KwietDsp*);
// frames = buffer interleaved f32, ~480 frames/appel
// retourne 0 = ok, <0 = erreur (le shim passe en passthrough)
int32_t   kwiet_dsp_process(KwietDsp*, const float* in, float* out, uint32_t frames);
void      kwiet_dsp_set_attenuation_db(KwietDsp*, float db); // thread-safe, atomic
```

Tout ce qui alloue vit dans create/destroy. `kwiet_dsp_process` ne doit pas
allouer après warmup — à vérifier sur la crate `df` (préchauffer ou wrapper si
elle alloue).

## 6. Contrôle UI ↔ APO ✅ — implémenté

Bloc de mémoire partagée nommé, **atomiques 32 bits uniquement** : ni pointeur,
ni taille, ni longueur ne traverse la frontière. Layout : `apo/src/KwietControl.h`,
qui est **le contrat** avec l'UI — tout changement impose un bump de
`KWIET_CONTROL_VERSION`.

| Sens | Champs |
|---|---|
| UI → APO | `enabled` (bypass), `aggressivenessTenths` (0–1000, dixièmes de dB) |
| APO → UI | `streaming`, `generation`, `sampleRate`, `channels`, `latencyFrames`, `peakIn`, `peakOut`, `underruns`, `dspErrors`, `dspActive` |

### Qui crée le bloc, et pourquoi

**L'APO le crée, pas l'UI.** Créer un objet dans l'espace de noms `Global\`
exige `SeCreateGlobalPrivilege`, que le jeton d'un utilisateur interactif ne
possède pas — même administrateur, il est filtré. `audiodg` l'a. Le `Global\`
est indispensable : l'APO vit en session 0, l'UI dans la session interactive.

Conséquence de conception : le bloc **n'existe que pendant un flux**. L'UI
persiste donc ses réglages de son côté et les re-pousse à chaque changement de
`generation`, qui signale un nouveau flux.

### Descripteur de sécurité et modèle de menace

```
D:(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x0007;;;AU)S:(ML;;NW;;;ME)
```

L'étiquette d'intégrité **Medium** est nécessaire : sans elle, le label High
hérité d'un processus système interdirait toute écriture depuis une UI
ordinaire.

> **Menace assumée** : tout processus d'intégrité moyenne de la session peut
> couper l'effet ou changer l'agressivité. C'est une nuisance, pas une
> escalade — aucune valeur du bloc n'est utilisée comme pointeur, taille ou
> index, et **toutes sont bornées à la lecture** côté APO. À restreindre à
> `INTERACTIVE` ou à un groupe dédié si le produit l'exige un jour.

### Chemins de lecture/écriture

- **Worker** (hors RT) : relit `enabled` et `aggressivenessTenths` à chaque
  réveil, ne pousse au moteur que sur changement effectif.
- **Thread temps-réel** : publie `peakIn`/`peakOut`. Un balayage linéaire de
  ~1000 flottants et deux `store` relaxés — pas d'appel système, donc conforme
  aux règles du chemin RT. `peakIn` est mesuré **avant** le DSP, car avec
  `APO_FLAG_INPLACE` le tampon d'entrée peut être celui de sortie.
- **Deux interrupteurs indépendants** : l'effet est actif seulement si Windows
  (SE3) *et* l'UI le veulent.

Banc d'essai : `bench/kwiet-control.ps1` lit et pilote le bloc **sans
élévation** — c'est ce qui valide l'ACL en pratique.

> ⚠️ **Piège corrigé le 2026-08-04.** L'ACL initiale n'accordait aux
> utilisateurs que `0x0007` (query + map read + map write). Tant que la section
> n'existait pas, l'APO la **créait** et tout fonctionnait. Dès que l'UI la
> garde ouverte — ce qu'elle fait exprès pour préserver les réglages entre deux
> flux — l'APO doit l'**ouvrir**, et `CreateFileMapping` réclame alors
> `SECTION_ALL_ACCESS` : `ERROR_ACCESS_DENIED`, plus de plan de contrôle
> (`control=0` dans le log). Corrigé en accordant `SECTION_ALL_ACCESS` aux
> utilisateurs authentifiés et le contrôle total aux comptes de service, plus
> un repli sur `OpenFileMapping` pour les sections héritées d'une version
> antérieure. Le droit plus large ne coûte rien : qui peut mapper la page en
> lecture/écriture en maîtrise déjà tout le contenu.

### ✅ Validé sur machine (2026-08-03)

Depuis un processus utilisateur **non élevé**, flux de capture actif :

| Action | Observation |
|---|---|
| Lecture d'état | `flux=actif dsp=on effet=on agress=100 dB 48000 Hz/2ch lat=1440 frames gen=1 under=0 err=0` |
| Agressivité 100 → 25 dB | VU sortie remonte de **−90,3 → −76,3 dB**, immédiatement, **sans redémarrage audio** |
| Bypass | VU sortie devient **exactement égale** à l'entrée (−69,5 / −69,5) |
| Retour à 30 dB | VU sortie repasse sous l'entrée |
| VU pendant la parole | entrée −69,5 → −54,3 dB, sortie suit à −59,7 dB |

`underruns=0` sur toute la séance. L'écriture sans élévation confirme le
descripteur de sécurité, et l'égalité parfaite des VU en bypass confirme que
le chemin de contournement est réellement transparent.

**C'est le verrou qui bloquait l'UI : le réglage est désormais instantané.**
Chaque essai d'agressivité coûtait jusqu'ici un redémarrage de la pile audio.

## 7. Détails d'implémentation du shim (jalon 1)

| Sujet | Décision |
|---|---|
| Cible | x64 uniquement (audiodg 64 bits) ; ARM64 plus tard |
| CRT | statique `/MT` — pas de dépendance VCRedist dans audiodg |
| COM | implémentation directe sans ATL, **agrégeable** (pattern inner/outer : audiodg crée les APO avec `pUnkOuter` non nul et `riid=IID_IUnknown` ; `CLASS_E_NOAGGREGATION` = éviction silencieuse) ; `ThreadingModel=Both` ; enregistrement sous `HKLM\Software\Classes\CLSID` |
| Catalogue APO | `HKLM\Software\Classes\AudioEngine\AudioProcessingObjects\{CLSID}` écrit par `DllRegisterServer` (équivalent du `RegisterAPO()` des samples : FriendlyName, Flags, connexions, `APOInterfaceN`) — lu par AudioEndpointBuilder à la découverte |
| Interfaces | `IAudioProcessingObject[RT\|Configuration]` + contrat Windows 11 complet : `IAudioSystemEffects3` (requis, sinon éviction), `IAudioProcessingObjectNotifications` (0 souscription), `IAudioProcessingObjectPreferredFormatSupport` (pas de préférence). Une fois SE3 exposé, `Initialize` reçoit `APOInitSystemEffects3` (80 octets) et non plus SE2 |
| Flags APO | `APO_FLAG_INPLACE \| APO_FLAG_DEFAULT` — tous les APO opérationnels constatés (Realtek, Microsoft) portent INPLACE ; `APOProcess` gère src==dst |
| Formats | float32 interleaved uniquement (`KSDATAFORMAT_SUBTYPE_IEEE_FLOAT`) |
| `GetEffectsList`/SE3 | expose `KWIET_EFFECT_NoiseSuppression` (contrôlable via `SetAudioSystemEffectState`) — un APO sans effet déclaré n'a aucune raison d'être inséré |
| `GetLatency` | 0 au jalon 1 ; retard fixe du ring au jalon 2 |
| DLL | **`C:\Windows\System32\KwietApo.dll`** — audiodg résout les APO par NOM DE FICHIER via l'ordre de recherche système (System32, System, Windows, PATH…), jamais par le chemin `InprocServer32` (constaté au procmon). Convention INF des APO (`DestinationDirs=11`) |
| SDK | header `IAudioSystemEffects3`/`APOInitSystemEffects3` extrait dans `apo/src/KwietSe3.h` (le SDK 19041 local prédate ces types) ; référence complète vendored dans `apo/third_party/winsdk/` (win32metadata, MIT) |
| APO non signé | `HKLM\...\CurrentVersion\Audio\DisableProtectedAudioDG = 1` en dev, retiré à la désinstallation ; disparaîtra avec la signature SignPath |
| Écriture FxProperties | .NET `OpenSubKey` à droits minimaux (`SetValue\|QueryValues`) : l'ACL de MMDevices n'accorde pas `KEY_WRITE` aux administrateurs (pas de `CreateSubKey`), donc `New-ItemProperty` échoue en accès refusé — constaté sur machine réelle |

## 7bis. Constats de terrain — Windows 11 build 26200 (jalon 1, machine réelle)

Séquence observée (log dev de la DLL + procmon) sur un endpoint de capture USB
(Plantronics), APO enregistré en MFX (`,6`) :

1. AudioEndpointBuilder lit les valeurs FX, interroge le catalogue APO, puis
   audiodg charge la DLL (une fois placée dans System32) et crée les instances
   **par agrégation**.
2. ~66 instanciations *discovery* (`InitializeForDiscoveryOnly=1`, mode SPEECH)
   avec `GetEffectsList` + `GetApoNotificationRegistrationInfo`.
3. **2 instanciations réelles** (`discoveryOnly=0`, modes DEFAULT et
   COMMUNICATIONS) : `Initialize` S_OK → QI des interfaces optionnelles (AEC,
   CustomFormats, Notifications2, une interface non documentée
   `{69E1F79F-...}`) → `GetPreferredOutputFormat` → **destruction immédiate**.
   Jamais de `IsInputFormatSupported` ni `LockForProcess`, quel que soit le
   retour de `GetPreferredOutputFormat` (S_OK+format ou E_NOTIMPL) et quels que
   soient les slots essayés (MFX, SFX, LFX, EFX, CompositeFX `,13`).
4. Les effets réellement actifs sur les endpoints de cette machine proviennent
   d'un **effect pack logiciel** (`SWD\DRIVERENUM\...VocaEffectPack`, propriété
   `{9e6136e0-...},100` sur l'endpoint) : les clés `CompositeFX` (`,13`/`,14`)
   ne sont qu'un reflet — les modifier à la main est sans effet sur le graphe.

Hypothèse confirmée : sur ce build, l'insertion d'APO tiers « registry-only »
dans les pipes de capture est refusée en fin de négociation. Le chemin moderne
est le **pack d'effets** — voir §8.

## 8. Pack d'effets (effect pack) — voie moderne ✅

Rétro-ingénierie du pack **Voice Clarity** de Microsoft (`C:\Windows\INF\oem155.inf`,
alias `voiceclarityep_audio_component.INF`) et confrontation au projet open
source [Aec3APO](https://github.com/msdx321/Aec3APO), qui applique la même
recette. C'est le mécanisme retenu pour Kwiet ; `installer/effectpack/` en est
l'implémentation.

Un pack d'effets, c'est **deux INF** :

1. **INF d'extension** (`Class=Extension`, sur `COMPUTER\Generic`) : sa seule
   fonction est `AddComponent` avec un `ComponentIDs = VEN_xxx&AUDIO_EFFECTPACK_yyy`,
   qui crée un devnode logiciel enfant `SWC\...`.
2. **INF de composant** (`Class=AudioProcessingObject`,
   ClassGuid `{5989fce8-9cd0-467d-8a6a-5419e31529d4}`) qui matche ce
   `SWC\VEN_xxx&AUDIO_EFFECTPACK_yyy`, copie la DLL dans le DriverStore
   (`DestinationDirs = 13`) et écrit **en HKR** (donc sous
   `HKLM\SYSTEM\CurrentControlSet\Control\Class\{5989fce8-...}\NNNN`) :
   - `Classes\CLSID\{APO}` + `InProcServer32 = %13%\xxx.dll` (REG_EXPAND_SZ) ;
   - `AudioEngine\AudioProcessingObjects\{APO}` (le catalogue, scopé composant) ;
   - `EffectPackRegistration\{EFFECT_CLSID}\FxProperties` : c'est **là** que
     vivent `PKEY_FX_ModeEffectClsid`, `PKEY_CompositeFX_ModeEffectClsid`,
     les modes supportés, et surtout les clés de ciblage.

Clés de ciblage qui remplacent l'édition manuelle des endpoints :

| PKEY | Valeur | Rôle |
|---|---|---|
| `PKEY_FX_ApplyToCapture` (`{D04E05A6-…},33`) | `1` | applique le pack aux endpoints de **capture** |
| `PKEY_FX_Enumerator` (`,23`) | `*` | tous les bus (USB, HDAUDIO, BTHENUM…) |
| `PKEY_FX_Association` (`,0`) | liste de `KSNODETYPE_*` | types de jack visés (micro, casque, array…) |
| `PKEY_FX_ExcludeForHWIDs` (`{6473F77A-…},2`) | motifs | exclusions (`ROOT\*`, périphériques virtuels) |
| `PKEY_FX_EffectPackSchema_Version` (`,29`) | `{7abf23d9-727e-4d0b-86a3-dd501d260101}` | schéma interne V1 |

C'est AudioEndpointBuilder qui, à partir de ces règles, **pousse lui-même** le
CLSID dans les `FxProperties` des endpoints concernés et y dépose un marqueur
`{CLSID_APO},100 = <InstanceId du devnode SWC>` (constaté sur l'endpoint casque
avec Voice Clarity). D'où la règle : **ne jamais écrire à la main dans les
FxProperties d'un endpoint** — c'est le pack qui décide, et une écriture
manuelle écrase la valeur du pack (le marqueur `,100` disparaît).

Signature : `pnputil /add-driver` accepte un package signé par un certificat
**auto-signé** placé dans `LocalMachine\Root` + `TrustedPublisher`, sans mode
test signing (validé sur cette machine).

Pour la distribution, un **certificat de signature de code commercial** ordinaire
devrait suffire — pas de signature attestation. Les deux régimes sont distincts,
et je les avais confondus :

| Régime | Ce qu'il exige | Nous concerne ? |
|---|---|---|
| [Signature à l'installation PnP](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/pnp-device-installation-signing-requirements--windows-vista-and-later-) | catalogue signé « par WHQL **ou** par un certificat de release tiers (SPC ou certificat commercial) » | **oui** — c'est ce qui gouverne `pnputil /add-driver` |
| [Politique de signature des pilotes](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/kernel-mode-code-signing-policy--windows-vista-and-later-) (Dev Portal, EV, depuis 1607) | signature Microsoft pour qu'un binaire **noyau** puisse se charger | **non** — Kwiet n'a aucun binaire noyau, c'est une DLL COM usermode |

Réserves : jamais tenté avec un certificat commercial sur une machine propre, et
Windows en mode S exige WHQL quelle que soit la classe.

#### Ce que fait un projet comparable

[`msdx321/Aec3APO`](https://github.com/msdx321/Aec3APO) est un pack d'effets
tiers de même nature (APO usermode, INF composant + extension). Son
`installer/sign-install.ps1` fait **exactement** ce que nous faisons : il crée un
certificat auto-signé, l'ajoute à `LocalMachine\Root` **et**
`LocalMachine\TrustedPublisher`, signe le catalogue, puis appelle
`pnputil /add-driver … /install`. Ni mode test-signing, ni certificat
commercial, ni Partner Center — et son paquet s'installait bien sur cette
machine. C'est une confirmation indépendante qu'aucune attestation n'est en jeu
pour un paquet sans binaire noyau.

Deux enseignements de comparaison :

- **À reprendre** : ils **horodatent** leur signature (`/tr … /td SHA256`). Sans
  horodatage, la signature devient invalide dès l'expiration du certificat, y
  compris pour les paquets déjà posés chez les utilisateurs. Adopté.
- **À ne pas reprendre pour une diffusion publique** : planter un certificat
  auto-signé dans `LocalMachine\Root` installe une **autorité racine** sur la
  machine de l'utilisateur. Cette racine peut ensuite cautionner n'importe quoi
  — tout binaire, et tout certificat TLS pour n'importe quel domaine. C'est
  acceptable sur une machine de développement dont on maîtrise le cycle de vie ;
  ça ne l'est pas dans un installeur distribué à des inconnus, et le README
  d'Aec3APO ne le signale pas. La voie propre reste le certificat de release
  tiers, qui chaîne vers une racine publique déjà présente : rien à ajouter au
  magasin.

### Cycle de vie observé (trace procmon, build 26200)

Au démarrage d'AudioEndpointBuilder, pour **chaque** pack installé (Voice
Clarity et Kwiet sont traités à l'identique) :

1. énumération de `...\Class\{5989fce8-…}\NNNN\EffectPackRegistration` ;
2. lecture complète des `FxProperties` du pack (association, enumerator,
   apply-to-capture, exclude-HWIDs, modes, MFX/composite CLSID, UI CLSID —
   toutes en SUCCESS) ;
3. inscription dans un registre central des packs :
   **`HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\EffectsPacks\{EFFECT_CLSID}`**
   avec `{D04E05A6-…},27` = InstanceId du devnode `SWD\DRIVERENUM\…` et
   `{D04E05A6-…},28` = 0.

Un pack **appliqué** à un endpoint se matérialise ensuite en **sous-clé** de
`MMDevices\Audio\Capture\{endpoint}\FxProperties` nommée d'après le
`MFX_CONTEXT` du pack, contenant `Default` et `User` (attention : ce sont des
sous-clés, pas des valeurs — un `Get-ItemProperty` sur `FxProperties` ne les
montre pas).

### État de validation (2026-08-03)

> ✅ **RÉSOLU (2026-08-03, 19h)** — voir « Sélection du pack » ci-dessous : il
> manquait uniquement la sélection du pack dans l'UI Son. L'APO Kwiet est
> désormais **chargé et actif** dans audiodg.

Le pack Kwiet est **accepté et suivi par le système exactement comme celui de
Microsoft** :

| Vérification | Kwiet | Voice Clarity |
|---|---|---|
| `pnputil /add-driver` (certificat auto-signé, sans test signing) | OK | — |
| devnode `SWD\DRIVERENUM\…#…EFFECTPACK` | OK | OK |
| composant `Class\{5989fce8-…}\NNNN` + 3 sous-clés | `0005` | `0001` |
| `EffectPackRegistration` lue intégralement par AudioEndpointBuilder | oui | oui |
| inscription dans `MMDevices\EffectsPacks\{CLSID}` (`,27`/`,28`) | oui | oui |
| **contexte matérialisé sur un endpoint** | **non** | oui (casque uniquement) |
| DLL chargée dans audiodg | non | **non plus** |

Deux constats qui réorientent le diagnostic :

- `voiceclaritycpuapo.dll` **n'est pas chargée non plus** pendant un flux de
  capture sur le casque, alors que les APO Realtek le sont (via la chaîne
  `CompositeFX` classique de l'endpoint). Le pack de Microsoft est donc
  *installé et sélectionné* mais son APO n'est pas actif — l'effet est
  vraisemblablement désactivé côté utilisateur.
- Voice Clarity n'est matérialisé que sur **un seul** endpoint (le casque),
  jamais sur le micro interne, alors que ses règles d'association couvrent les
  deux. Ni un redémarrage, ni `pnputil /scan-devices`, ni un cycle
  désactivation/réactivation des endpoints ne provoquent la matérialisation du
  pack Kwiet.

### Sélection du pack — la pièce manquante ✅

« MEP » = *Multiple Effect Packs*. Un pack installé n'est **pas** appliqué
automatiquement : il devient une **option** dans
`Paramètres > Système > Son > [micro] > Améliorations audio`, et l'utilisateur
en choisit **un seul** par endpoint. C'est la raison d'être de
`PKEY_FX_MEP_UserInterfaceClsid` : sans cette valeur, le pack n'est pas
proposé dans la liste.

Après sélection de « Kwiet » dans cette liste, l'endpoint reçoit :

```
FxProperties\{MFX_CONTEXT}\Default   <- description + état de l'effet
FxProperties\{MFX_CONTEXT}\User      <- choix utilisateur
FxProperties\{CLSID_APO},100 = SWD\DRIVERENUM\…#KwietEffectPack…
```

Ce qui explique aussi pourquoi Voice Clarity n'était matérialisé que sur le
casque : c'était le pack sélectionné pour **cet** endpoint, et un seul pack
peut l'être à la fois. En choisissant Kwiet, on le remplace (réversible via la
même liste).

### ✅ Jalon 1 atteint (2026-08-03, build 26200)

Log dev de l'APO chargé dans `audiodg.exe`, sur le micro USB Plantronics :

```
LockForProcess: S_OK, 2 ch, 48000 Hz
… (traitement) …
UnlockForProcess
KwietApo: instance destroyed
```

- **8 cycles Lock/Unlock** consécutifs, tous propres ;
- **130 instances créées / 130 détruites** — aucune fuite de refcount ;
- capture ffmpeg pendant que l'APO est actif : `mean_volume −63.7 dB`,
  `max_volume −42.8 dB` → signal réel transmis, **pas de silence numérique**,
  passthrough fonctionnel ;
- aucun crash d'audiodg, audio système intact.

Reste à faire pour clore formellement le jalon 1 : le soak 48 h et la matrice
de robustesse (changement de sample rate, débranchement à chaud, veille/reprise,
multi-applis) décrits dans `procedure-test-vm.md`.

## 9. ✅ Jalon 2 — plomberie DSP validée (2026-08-03)

### Ce qui tourne

`APOProcess` ne fait plus que pousser le quantum capturé dans un ring SPSC et
en dépiler un déjà traité. Entre les deux, un worker à priorité normale est le
seul à appeler le DSP.

| Élément | Mesure |
|---|---|
| Format négocié | 48 kHz, 2 ch, quantum 480 frames (10 ms) |
| Latence du pipeline | 1440 frames = **30 ms**, remontée par `GetLatency` (300000 hns) |
| Stabilité | `underruns=0 overruns=0 dspErrors=0` sur tous les flux mesurés |

### Preuve que le DSP est réellement dans le chemin

Deux mesures acoustiques successives, **source contrôlée** (bruit blanc joué
dans le casque), sans réinstaller le pack entre les états — seul le réglage
d'atténuation change à chaud :

| Atténuation | Prises | Moyenne |
|---|---|---|
| 0 dB (identité) | −58,9 / −54,5 / −62,7 dB | −58,7 dB |
| −40 dB | −91,0 / −91,0 / −91,0 dB | −91,0 dB |

Les trois prises à −40 dB tombent **exactement** sur −91,0 dB : c'est le
plancher de quantification du 16 bits de la capture dshow. L'écart apparent
(−32,3 dB) est donc borné par l'instrument de mesure, pas par le DSP.

> ⚠️ Méthode : toute mesure basée sur le **bruit ambiant** est inexploitable —
> 12 dB de variation entre prises consécutives, 25 dB entre sessions. Il faut
> une source contrôlée. Deux conclusions antérieures tirées du bruit ambiant se
> sont révélées être des coïncidences.

### Décisions d'implémentation

| Sujet | Décision et raison |
|---|---|
| Chargement du cdylib | `LoadLibraryEx` par **chemin absolu** dérivé de `GetModuleFileName(g_kwietModule)` : le DriverStore n'est pas dans l'ordre de recherche système, un `LoadLibrary("kwiet_dsp.dll")` échouerait |
| CRT Rust | **statique** (`dsp/.cargo/config.toml`) : sinon la DLL importe `VCRUNTIME140.dll`, absente d'une machine sans VCRedist — le chargement échouerait dans audiodg |
| Panics Rust | `catch_unwind` à chaque point d'entrée, **jamais `panic = "abort"`** : un abort tuerait audiodg, donc tout le son de la machine |
| Version d'ABI | `kwiet_dsp_abi_version()` vérifiée avant tout appel — garde contre une DLL périmée laissée dans le DriverStore |
| Bypass de l'effet | appliqué **dans le worker**, pas en court-circuitant le pipeline : la latence reste constante, donc la valeur annoncée par `GetLatency` (interrogée une seule fois par flux) reste valide |
| Amorçage | le ring de sortie est pré-rempli de silence à hauteur de la latence : c'est le retard algorithmique, pas un mode dégradé |
| `BUFFER_SILENT` | ni push ni pop : les rings gardent leur niveau de remplissage et l'audio reprend sans underrun |
| Réglage dev | `HKLM\SOFTWARE\Kwiet\AttenuationDbTenths` (REG_DWORD signé, dixièmes de dB), lu à `LockForProcess`, **compilé uniquement avec `KWIET_DEV_LOG`**. Provisoire jusqu'au plan de contrôle shmem du jalon 4 |

### Pièges de packaging (coûteux, à ne pas réapprendre)

1. **`pnputil` compare les packages sur `DriverVer`, pas sur leur contenu.** Une
   DLL recompilée sans bump de version n'est jamais redéployée (« package déjà
   à jour »), et on mesure l'ancienne sans s'en apercevoir.
   `sign-install-dev.ps1` réécrit donc une version monotone (`0.2.MMjj.HHmm`)
   dans la copie packagée à chaque installation.
2. **Mettre à jour le pack fait perdre la sélection de l'endpoint.** Le marqueur
   `{CLSID_APO},100` disparaît, plus aucun pack n'est actif, et l'APO cesse
   d'être chargé — silencieusement. Il faut re-choisir le pack dans
   Paramètres > Son. **À gérer explicitement dans l'installeur final.**
3. `pnputil` renvoie **259** (`ERROR_NO_MORE_ITEMS`) quand il n'y a rien à
   faire : ce n'est pas une erreur.

### Loose end

`SystemSettings.exe` appelle `DllGetClassObject` sur le CLSID « Settings
manager » (`PKEY_FX_MEP_UserInterfaceClsid`) que l'INF déclare mais que la DLL
n'implémente pas — on lui renvoie `CLASS_E_CLASSNOTAVAILABLE`. Sans conséquence
visible aujourd'hui, mais c'est le point d'entrée que Windows offre pour l'UI
de réglage : à implémenter au jalon 4 plutôt que d'inventer un autre canal.

## 10. ✅ DeepFilterNet3 intégré (2026-08-03)

Le gain de test du jalon 2 est remplacé par DFN3 (crate `deep_filter`, backend
tract/ONNX). Le modèle est **embarqué dans la DLL** (`DfParams::default()`,
feature `default-model`) : `kwiet_dsp.dll` fait 25 Mo et ne dépend d'aucun
fichier externe ni d'aucune DLL hors système.

### Mesures locales (tests du crate)

| Mesure | Résultat |
|---|---|
| Facteur temps-réel, bruit | **0,017** |
| Facteur temps-réel, tonalité | **0,020** |
| Bruit large bande résiduel | **0,0 %** |
| Tests / clippy | 14 verts, `-D warnings` propre |

Le RTF est mesuré **sur du bruit** autant que sur une tonalité : DFN3
court-circuite ses étages lourds quand le SNR local est élevé
(`apply_stages()`), donc un signal propre flatterait le résultat. ~2 % d'un
cœur laisse 50× de marge sur le budget de 10 ms par bloc.

### Validation sur machine (A/B, source contrôlée)

Bruit blanc joué dans le casque, pack sélectionné, aucune réinstallation entre
les états — seul le réglage d'agressivité change à chaud :

| Suppression | Prises | Moyenne |
|---|---|---|
| 0 dB (désactivée) | −63,9 / −63,8 / −63,9 dB | −63,9 dB |
| 100 dB (maximale) | −91,0 / −91,0 / −81,3 dB | −87,8 dB |

**Réduction ≥ 24 dB**, avec `underruns=0 overruns=0 dspErrors=0` dans les deux
états, `block=480` (le hop imposé par DFN3), latence inchangée à 1440 frames.

À noter méthodologiquement : l'état à 0 dB donne trois prises à 0,1 dB près,
là où les mesures sur bruit ambiant variaient de 12 dB. La source contrôlée
est ce qui rend ces chiffres exploitables. Deux des trois prises à 100 dB
touchent le plancher 16 bits (−91,0 dB), donc la suppression réelle est
supérieure à la valeur affichée.

### Test voix + bruit (le seul qui compte vraiment)

Bruit rose joué dans le casque, prise de 12 s segmentée : 4 s de silence pour
mesurer le plancher de bruit, puis 7 s de parole. Même protocole dans les deux
états, seul le réglage d'agressivité change.

| | Sans suppression | Avec DFN3 |
|---|---|---|
| Bruit de fond | −40,4 dB | **−91,0 dB** |
| Voix | −30,3 dB | −32,1 dB |
| SNR | 10,1 dB | 58,9 dB |

- **bruit supprimé : ≥ 50 dB** (le plancher 16 bits est atteint, donc borné
  par la mesure) ;
- **voix conservée à 1,8 dB près** — dans la variation naturelle entre deux
  prises parlées ;
- **gain de SNR ≈ 49 dB**, `underruns=0` dans les deux états.

> ⚠️ Ces chiffres mesurent des **niveaux**, pas la **qualité**. Une voix peut
> conserver son niveau tout en étant abîmée (artefacts, sifflements,
> pompage). Seule l'écoute tranche, et c'est ce qui doit guider le jalon 3.
> Le protocole dépend aussi de la constance de l'élocution entre les prises.

### Changements d'ABI (v2)

| Élément | Décision |
|---|---|
| `kwiet_dsp_block_frames()` | **nouveau** — DFN3 impose 480 frames (10 ms) par inférence ; l'hôte s'en sert comme taille de bloc du worker, les rings découplant ça du quantum de l'APO |
| `kwiet_dsp_set_attenuation_db()` | **sémantique changée** — n'est plus un gain sur le signal mais la limite maximale d'atténuation du bruit (0 = aucune suppression, 100 = illimitée, défaut du modèle). C'est le contrôle « agressivité » |
| Format | 48 kHz **uniquement** : `create` renvoie NULL sinon, l'hôte reste en passthrough. Resampling à faire |
| Canaux | modèle mono : somme des canaux en entrée, résultat réécrit sur tous les canaux en sortie |
| Sûreté | `DfTract` n'est pas `Sync` : l'état du modèle vit dans un `UnsafeCell` touché uniquement par `process` (contrat mono-worker), les contrôles restent des atomiques relues au bloc suivant — pas d'aliasing `&`/`&mut` |
| Allocation | `process` **alloue** (tract alloue par inférence). Acceptable : il tourne sur le worker, jamais sur le thread RT — c'est exactement pourquoi l'architecture l'y a placé |

### Épinglages de dépendances obligatoires

1. **Famille `tract` en `=0.21.4`.** `deep_filter` déclare `^0.21.4`, mais tract
   a renommé un champ public (`symbol_table` → `symbols`) dans une version que
   cargo juge compatible ; résolu en 0.21.17, `deep_filter` ne compile plus.
   L'épinglage doit être fait **dans le manifeste** : `cargo update --precise`
   rétrograde crate par crate et casse la cohérence interne de la famille
   (`tract-pulse-opl` exige `=` sa propre version).
2. **`kstring` en `2.0.2`** — la 2.0.4 exige rustc 1.96, la toolchain est en 1.90.

### Reste à faire

- **Resampling** pour les endpoints qui ne négocient pas 48 kHz (aujourd'hui :
  passthrough silencieux).
- **Latence réelle** : les 30 ms du ring sont remontés à `GetLatency`, mais le
  lookahead propre à DFN3 n'y est pas ajouté — à quantifier et à déclarer.
- **Qualité sur la voix** : tout ce qui précède mesure la suppression de bruit
  pur. L'effet sur la parole (et le bench AGC Chrome du jalon 3) reste entier.

## 11bis. ✅ RÉSOLU — Kwiet traite bien le micro dans Google Meet

**Preuve fonctionnelle, obtenue depuis Meet lui-même :** l'indicateur de niveau
de Meet réagit à la voix mais **plus au sifflement**, alors qu'il y réagissait
avant l'installation. DeepFilterNet3 préserve la parole et supprime le reste :
un sifflement n'est pas de la parole, il est retiré. Le traitement est donc
appliqué au flux que Meet reçoit.

Confirmé côté APO : `LockForProcess` sans `Unlock` (graphe verrouillé en
permanence), `flux=actif` dans le bloc de contrôle, VU d'entrée qui suit la
parole et sortie écrasée dans les silences, `underruns=0`.

### Deux erreurs de méthode qui ont coûté des heures

1. **« 0 nouvelle ligne de log » ne prouve rien.** L'APO ne journalise qu'aux
   transitions (Initialize, Lock, Unlock) : un flux **déjà verrouillé** n'écrit
   plus rien tant qu'il tourne. Compter les lignes ajoutées sur une fenêtre de
   8 s a donc produit des faux négatifs répétés. Le bon indicateur est
   `streaming` dans le bloc de contrôle — c'est-à-dire le panneau lui-même.
2. **« DLL chargée dans audiodg » ne prouve rien non plus** : une DLL reste
   mappée après usage. C'est ce qui a rendu le verdict sur Aec3APO peu fiable
   (même s'il allait dans le bon sens).

L'utilisateur avait raison avant moi : « tant qu'on ne voit pas un flux ouvert
dans le tray, ça ne marche pas » était le bon instrument depuis le début.

### Ce qui a débloqué (avec une réserve honnête)

Trois changements ont été appliqués en séquence sans mesure fiable entre eux :
RAW réintroduit dans les modes, `IAudioProcessingObjectPreferredFormatSupport`
retirée du QI, et `u32NumAPOInterfaces` ramené de 3 à **1** pour s'aligner sur
Voice Clarity et Aec3APO. **On ne sait pas lequel est décisif**, et vu les faux
négatifs, il est possible que le blocage ait cédé plus tôt.

Un quatrième candidat a en revanche été **éliminé par la mesure** : retirer
`IApoAcousticEchoCancellation` ne change rien, le flux de Meet continue de
passer par l'APO. Se déclarer annuleur d'écho n'était donc pas nécessaire.

### Décision : Kwiet ne fait pas d'annulation d'écho ✅

Réclamer `IApoAcousticEchoCancellation` fait **désactiver à Chrome son propre
AEC3**, l'annuleur de WebRTC, qui est état de l'art. Le remplacer par ce qu'on
pourrait livrer (SpeexDSP) serait une régression nette pour l'utilisateur et
ses interlocuteurs. Le métier de Kwiet est la suppression de bruit ; l'écho
revient à l'application, qui le fait mieux.

Les interfaces d'entrée auxiliaire sont **conservées** : le flux de référence
ne coûte rien et sert au diagnostic (`refFrames` dans le log).

Conséquence pratique : tout le chantier SpeexDSP est annulé. Pour mémoire, il
aurait fallu vendorer les sources C et écrire un build `cc` maison — le module
`echo` du crate `speexdsp` est entièrement derrière la feature `sys`, et
`speexdsp-sys` passe par autotools et pkg-config, inutilisables sur MSVC.

> ⚖️ **Licence** : le dépôt Aec3APO ne contient **aucun fichier de licence**,
> donc tous droits réservés. Son code peut être lu pour comprendre, jamais
> copié ni dérivé. Il n'a servi que de témoin expérimental.

Note : le graphe qui aboutit est initialisé en mode **DEFAULT**, pas
COMMUNICATIONS — le traitement s'applique quand même au flux de Meet. Pourquoi,
reste une curiosité non élucidée, plus un blocage.

## 11. Chrome et le contournement des APO (historique du diagnostic)

**Constat, 2026-08-04.** Pendant un Google Meet, le panneau affichait « aucun
micro actif ». Diagnostic :

| Vérification | Résultat |
|---|---|
| Le Plantronics capte-t-il ? | oui, pic à −58,3 dB (`IAudioMeterInformation`) |
| Kwiet est-il sélectionné sur cet endpoint ? | oui |
| L'APO est-il instancié ? | **non** — aucune ligne de log, `streaming=0` |
| Un flux **normal** sur le **même** micro, **pendant** le Meet ? | **l'APO se charge** (32 lignes de log) |

Donc ni l'endpoint, ni la sélection, ni l'APO ne sont en cause : c'est le flux
de Chrome qui saute la chaîne d'effets.

### Mécanisme

Chromium active par défaut la fonctionnalité **`WASAPIRawAudioCapture`**, qui
ouvre la capture avec
[`AUDCLNT_STREAMOPTIONS_RAW`](https://learn.microsoft.com/en-us/windows/desktop/api/audioclient/ne-audioclient-audclnt_streamoptions).
Ce drapeau « bypasses all signal processing except for endpoint specific,
always-on processing » — donc tous les APO logiciels, y compris le nôtre et
Voice Clarity de Microsoft. WebRTC veut du signal brut pour appliquer ses
propres AEC/NS/AGC.

### Contournement testé — et qui NE marche PAS

```
chrome.exe --disable-features=WASAPIRawAudioCapture
```

Vérifié appliqué au processus utilitaire audio de Chrome (`--type=utility`,
sous-type audio). **Aucun effet** : micro à −28,2 dB pendant le Meet, log de
l'APO inchangé à la ligne près.

### Le mécanisme réel (source Chromium, `audio_low_latency_input_win.cc`)

`SetCommunicationsCategoryAndMaybeRawCaptureMode()` appelle
`IAudioClient2::SetClientProperties` avec `AudioCategory_Communications` et :

```cpp
if (channels > 0 && channels <= kMaxRawCaptureChannels)
    audio_props.Options = AUDCLNT_STREAMOPTIONS_RAW;
...
if (aec_config_) {
    audio_props.Options = AUDCLNT_STREAMOPTIONS_NONE;
    // "(WARNING: attempting to enable system AEC)"
}
```

Deux enseignements décisifs :

1. Le mode raw est conditionné à `raw_processing_supported_`, lu depuis la
   propriété d'endpoint **`System.Devices.AudioDevice.RawProcessingSupported`**
   — donc **c'est le périphérique qui autorise Chrome à contourner**, pas
   seulement un réglage de Chrome.
2. Chrome **repasse explicitement en `AUDCLNT_STREAMOPTIONS_NONE`** — effets
   système actifs — dès qu'il veut l'**AEC système**. Le commentaire du code
   nomme Voice Clarity : ce chemin existe précisément pour que les effets de
   Microsoft fonctionnent.

### ✅ Cause réelle trouvée : le pipe COMMUNICATIONS nous éjecte

La piste A a été testée (RAW retiré de nos modes déclarés) et n'a rien changé —
l'endpoint continue d'annoncer RAW, qui vient donc du pilote et non de nous.
Mais le log a livré la vraie explication. **L'APO *est* instancié pour le flux
de Chrome, en mode COMMUNICATIONS, et il est détruit avant `LockForProcess` :**

```
Initialize: SE3, mode={98951333-...COMMUNICATIONS}, discoveryOnly=0
Initialize: S_OK
QI: E_NOINTERFACE for {4CEB0AAB-...}   IApoAuxiliaryInputConfiguration
QI: E_NOINTERFACE for {25385759-...}   IApoAcousticEchoCancellation
QI: E_NOINTERFACE for {F235855F-...}   IApoAcousticEchoCancellation2
GetPreferredOutputFormat -> E_NOTIMPL
KwietApo: instance destroyed            <-- pas de LockForProcess
```

Les instances en mode DEFAULT, elles, atteignent `LockForProcess` normalement
(171 instanciations DEFAULT contre 21 COMMUNICATIONS, aucune de ces dernières
n'aboutissant).

**Ce n'est donc pas Chrome qui nous contourne : c'est le moteur audio qui nous
refuse le pipe communications parce que nous ne fournissons pas d'AEC.** Chrome
ouvre en `AudioCategory_Communications` ; ce pipe attend un APO capable
d'annulation d'écho, et Voice Clarity en est un (son INF déclare
`APO_FLAG_AEC`). Cela explique aussi le commentaire du code Chromium : le
chemin « system AEC » existe pour ces effets-là.

Toutes les applications de visioconférence ouvrant en catégorie communications,
c'est **le** verrou produit.

### Deux pistes, par ordre de coût

**A. Ne plus déclarer le mode RAW.** Notre INF annonce
`AUDIO_SIGNALPROCESSINGMODE_RAW` dans
`PKEY_MFX_ProcessingModes_Supported_For_Streaming` — copié de Voice Clarity. Si
c'est ce qui fait remonter `RawProcessingSupported = true`, on se tire une
balle dans le pied : on annonce à Chrome que le raw est disponible, il le
prend, et notre MFX est contourné. Expérience peu coûteuse : retirer RAW,
redéployer, re-sélectionner, retester Meet.

**B. Se déclarer fournisseur d'AEC système.** C'est le chemin béni par le code
de Chromium. Concrètement : implémenter `IApoAcousticEchoCancellation`
(`{25385759-3236-4101-A943-25693DFB5D2D}`) et le flux de référence associé —
interfaces que `audiodg` nous demande déjà et auxquelles nous répondons
`E_NOINTERFACE` (visible dans nos logs depuis le jalon 1). Bien plus de
travail, mais c'est la voie que Microsoft a ouverte pour ses propres effets.

### Piste B, étape 1 : le contrat AEC (2026-08-04)

Trois interfaces extraites dans `apo/src/KwietAec.h` et implémentées :

| Interface | Rôle |
|---|---|
| `IApoAcousticEchoCancellation` | **marqueur, aucune méthode** — la présenter suffit à se déclarer annuleur d'écho |
| `IApoAuxiliaryInputConfiguration` | enregistrement du flux de référence (rendu), hors RT |
| `IApoAuxiliaryInputRT` | `AcceptInput()` livre l'audio de référence **sur le thread temps-réel**, séparément d'`APOProcess` |

Résultat mesuré : le moteur nous interroge et **accepte** ces interfaces, puis
appelle `AddAuxiliaryInput: id=1, 2 ch, 48000 Hz`. **Il nous enregistre donc
bien un flux de référence** — nous sommes reconnus comme APO capable d'AEC.

Mais l'instance qui reçoit ce flux et atteint `LockForProcess` est initialisée
en mode **DEFAULT**. Les instances **COMMUNICATIONS restent détruites**. Le
verrou n'est donc pas (seulement) le contrat AEC.

**Test refait micro actif (Meet en cours, micro à −10,5 dB) : zéro ligne de
log.** L'APO n'est pas même instancié pour ce flux. Implémenter le contrat AEC
ne suffit donc pas à entrer dans le pipe communications.

Candidats restants pour l'acceptation en COMMUNICATIONS :

- `IApoAcousticEchoCancellation2` (`{F235855F-...}`), toujours refusée faute de
  la structure `APO_REFERENCE_STREAM_PROPERTIES`, absente du header vendored ;
- `{69E1F79F-6EAE-4517-BE9F-13AA90E30014}`, interface non identifiée, demandée
  systématiquement et refusée ;
- les `Flags` d'enregistrement : Voice Clarity déclare `0x0C`
  (`FRAMESPERSECOND` + `BITSPERSAMPLE` doivent correspondre) là où nous
  déclarons `0x0F` — nous imposons en plus `INPLACE` et
  `SAMPLESPERFRAME_MUST_MATCH`, ce qui interdit au moteur de nous confier un
  mixdown multicanal → mono, exactement ce qu'un AEC fait.

Le dernier point est le plus suspect. `APO_FLAG_SAMPLESPERFRAME_MUST_MATCH`
signifie « ma sortie a autant de canaux que mon entrée ». Or le pipe
communications alimente un encodeur mono, et Voice Clarity produit
effectivement du mono depuis un micro multicanal — c'est précisément pourquoi
elle ne déclare pas ce drapeau. En l'imposant, nous nous excluons peut-être
nous-mêmes.

Ce n'est pas un changement d'une ligne : accepter une sortie mono implique que
`LockForProcess` tolère des formats d'entrée/sortie différents, et que
`DspHost` distingue canaux d'entrée et de sortie. Le moteur Rust, lui, est déjà
prêt — il somme déjà en mono en interne et ne fait que redupliquer ensuite.

**Testé, et négatif également.** `Flags` passés à `0x0C` (vérifiés lus par le
moteur : `Flags = 0xC`), conversion de canaux implémentée
(`apo/src/ChannelMix.h`, `MixChannels`), formats d'entrée/sortie découplés dans
`LockForProcess` et `DspHost`. Micro actif dans un Meet : **zéro ligne de log**.

> ⚠️ Piège rencontré en cours de route : le catalogue APO que le moteur lit est
> celui, **scopé au composant**, que pose l'INF — pas celui qu'écrit
> `DllRegisterServer` sous `HKLM\Software\Classes`. Changer les `Flags` dans le
> code C++ sans changer `APO_FLAGS` dans `kwiet_component.inf` n'a donc aucun
> effet, et invalide silencieusement le test. Les deux doivent rester en phase.

### Bilan des hypothèses testées

| Hypothèse | Résultat |
|---|---|
| Chrome `--disable-features=WASAPIRawAudioCapture` | sans effet |
| Retirer RAW de nos modes déclarés | sans effet (RAW vient du pilote) |
| Implémenter le contrat AEC (3 interfaces) | interfaces acceptées, `AddAuxiliaryInput` appelé… mais en mode DEFAULT seulement |
| `Flags` `0x0F` → `0x0C` + sortie mono possible | sans effet |

### ✅ Expérience de contrôle : Voice Clarity, elle, y arrive

Voice Clarity sélectionnée sur l'endpoint, Meet en cours, micro à −4,3 dB :
`voiceclaritycpuapo.dll` **est chargée dans `audiodg`**. Donc **un APO peut
entrer dans le pipe communications de Chrome** — la piste n'est pas un
cul-de-sac, il reste à trouver ce qui nous en distingue.

### Le diff qui répond

Comparaison exhaustive des deux enregistrements de composant
(`AudioEngine\AudioProcessingObjects` + `EffectPackRegistration`), GUID propres
à chaque pack normalisés. Hors cosmétique (noms, auteur, numéros de version),
il ne reste **qu'une seule différence fonctionnelle** :

```
PKEY_MFX_ProcessingModes_Supported_For_Streaming
  Voice Clarity : DEFAULT | COMMUNICATIONS | SPEECH | RAW
  Kwiet         : DEFAULT | COMMUNICATIONS | SPEECH
```

C'est précisément le `RAW` retiré en « piste A », sur une hypothèse démentie
depuis (le mode raw de l'endpoint vient du pilote, pas de nous). Restauré.

> Leçon de méthode : la piste A a été menée sur un raisonnement plausible mais
> non vérifié, et a introduit la seule divergence restante avec le pack de
> référence. Le diff systématique contre un pack qui fonctionne aurait dû venir
> avant toute modification spéculative.

**RAW restauré, vérifié enregistré (`DEFAULT | COMMS | SPEECH | RAW`), Meet en
cours, micro à −8,1 dB : toujours zéro ligne de log.**

L'enregistrement est donc désormais fonctionnellement identique à celui de
Voice Clarity, et pourtant elle entre dans le pipe et nous non. **La cause
n'est plus déclarative.** Il reste deux explications :

1. **Le comportement de la DLL.** À chaque instanciation, `audiodg` nous
   interroge et nous refusons trois interfaces :
   - `{F235855F-...}` `IApoAcousticEchoCancellation2` — Voice Clarity
     l'implémente très probablement. Son unique méthode
     `GetDesiredReferenceStreamProperties` demande la structure
     `APO_REFERENCE_STREAM_PROPERTIES`, absente du header vendored : il faut la
     récupérer d'un SDK ≥ 22000.
   - `{69E1F79F-6EAE-4517-BE9F-13AA90E30014}` — **non identifiée**, demandée
     systématiquement depuis le jalon 1, jamais élucidée. À chercher en
     priorité : c'est la seule inconnue franche du dossier.
   - `{B1176E34-...}` `IAudioSystemEffectsCustomFormats`.

2. **Le niveau de confiance.** ~~Voice Clarity est signée par Microsoft ; nous
   utilisons un certificat de test auto-signé. Le rôle d'annuleur d'écho du
   pipe communications exige peut-être une signature d'attestation, là où le
   pipe DEFAULT se contente d'un package accepté par `pnputil`.~~

   **Écartée** (2026-08-04). D'abord parce que Kwiet traite désormais le micro
   de Meet avec ce même certificat de test, sans rien changer à la signature —
   voir §11bis. Ensuite parce que l'hypothèse reposait sur une confusion :
   l'attestation relève de la politique de chargement des binaires **noyau**, et
   n'a rien à dire sur un APO usermode. Voir §8, tableau des deux régimes.

### L'expérience de contrôle qui manquait

Toutes ces hypothèses supposent que **Voice Clarity, elle, fonctionne avec
Meet**. Ce n'est pas vérifié. Il faut sélectionner Voice Clarity sur l'endpoint,
rejoindre un Meet et regarder si `voiceclaritycpuapo.dll` est chargée dans
`audiodg` :

- **si oui** → un APO peut bien entrer dans ce pipe, et il reste à trouver ce
  qui nous distingue d'elle ;
- **si non** → Chrome contourne *tous* les APO, y compris ceux de Microsoft, et
  toute cette piste est un cul-de-sac. La question devient alors produit :
  couvrir les applications qui n'utilisent pas WebRTC, ou revenir au micro
  virtuel écarté au départ.

Cette expérience aurait dû venir en premier.

### Ce que ça change pour le produit

`docs/architecture.md` notait depuis le début « les flux exclusive/raw
bypassent l'APO, c'est accepté ». Cette limite avait été acceptée **sans savoir
que Chrome en fait partie** — or Chrome/Meet est le cas d'usage n°1 du produit.
C'est donc une remise en cause sérieuse, découverte au moment où le jalon 3
(bench Chrome) devait précisément l'examiner.

Pistes, par ordre de préférence :

1. **Le commutateur ci-dessus** — s'il fonctionne, reste à savoir s'il est
   praticable pour un utilisateur ordinaire (raccourci modifié, stratégie
   d'entreprise, ou rien du tout).
2. **Vérifier les autres applications visées** (Discord, Teams, Zoom, Slack) :
   elles n'utilisent pas toutes WebRTC de la même façon, et la question
   « combien d'applis sont réellement couvertes » décide de la valeur du
   produit.
3. **Micro virtuel** — la solution qui fonctionne quoi qu'il arrive, mais elle
   a été explicitement écartée au départ (« sans micro virtuel »). À rouvrir
   seulement si 1 et 2 échouent.

## 12. ✅ Produit livrable — identité, détection, installeur (2026-08-04)

### Détecter un pack installé mais non sélectionné

C'est le piège qui a coûté le plus de temps sur ce projet : un pack installé
mais non choisi est **totalement inerte**, et rien dans la pile audio ne le
signale — ni erreur, ni ligne de log, ni propriété COM. Le panneau le détecte
désormais, à partir de deux faits écrits par Windows lui-même :

| Fait | Où le lire |
|---|---|
| Le pack est **installé** | une sous-clé de `HKLM\SYSTEM\CurrentControlSet\Enum\SWD\DriverEnum` nommée `{ExtensionId}#KwietEffectPack&…` |
| Le pack est **sélectionné** sur un endpoint | une valeur de `…\MMDevices\Audio\Capture\{guid}\FxProperties` dont le nom commence par **notre propre CLSID** |

Le second a été trouvé par observation : avec Kwiet sélectionné sur un micro et
pas sur l'autre, seul le premier porte
`{65d564e6-…},100 = SWD\DRIVERENUM\{…}#KwietEffectPack&…`. On matche donc sur le
**préfixe CLSID** et non sur l'identifiant de propriété `100`, dont on ignore la
sémantique — cette valeur est un détail d'implémentation non documenté.

Quel micro compte vient de COM (`GetDefaultAudioEndpoint(eCapture,
eCommunications)`), pas du registre. Cela permet de distinguer un troisième
état, qui autrement ressemble à un bug : **pack sélectionné sur un autre micro
que celui que Windows donne aux applications**.

`PackStatus` porte un drapeau `known`, sans quoi « pas encore regardé » et
« pas installé » seraient la même valeur et le panneau annoncerait un pack
absent pendant ses deux premières secondes.

> ⚠️ `ms-settings:sound-defaultinputproperties` **ne fait pas** ce que son nom
> promet : sur 26200 elle ouvre les propriétés d'un périphérique fantôme
> intitulé « Null description », sans section Améliorations audio. On ouvre
> `ms-settings:sound`, qui marche.

### Bug corrigé : le flux fantôme

L'UI gardait la section de contrôle mappée en permanence, pour conserver les
réglages entre deux flux. Une section nommée vit tant qu'**un** handle existe :
quand audiodg démontait l'APO sans appeler `UnlockForProcess` — ce qui arrive en
désélectionnant le pack — le bloc restait mappé avec `streaming` à 1. Le panneau
affichait alors une ligne d'état complète, aucun signal, et masquait l'écran qui
aurait expliqué pourquoi.

L'UI ouvre désormais la section **par appel**. L'existence de la section
signifie ce que le panneau lui fait dire : l'APO est chargé. Les réglages sont
repoussés par le thread de veille dès qu'il voit une nouvelle génération.

### Installeur

Bundle NSIS de Tauri, `perMachine` (le pack est un package pilote, donc
élévation), français et anglais. Le pack est embarqué comme ressource et posé
par `pack.ps1`, livré à côté de lui pour pouvoir réparer une installation à la
main. Les hooks `NSIS_HOOK_POSTINSTALL` / `PREUNINSTALL` ne font qu'orchestrer.

Trois pièges payés :

1. **L'installeur NSIS est 32 bits.** WOW64 redirige `System32` vers
   `SysWOW64`, où `pnputil.exe` n'existe pas : l'installation échoue sur un
   « commande introuvable » qui ne dit rien de la cause. Corrigé des deux côtés
   (`Sysnative` dans le script, `DisableX64FSRedirection` dans le hook).
2. **24 paquets Kwiet s'étaient accumulés** dans le magasin de pilotes, un par
   itération de dev. La désinstallation énumère et retire *tous* les paquets
   dont le nom d'origine est l'un des nôtres.
3. **`DriverVer` porte un quatrième champ à zéro.** La version publiée est la
   version. Y glisser une date casserait l'ordre au changement d'année.

Les noms publiés (`oemNN.inf`) sont relus depuis `pnputil /enum-drivers` plutôt
que depuis un fichier d'état : le magasin est la vérité, un fichier peut mentir.
Le découpage ne cherche que des noms de fichiers, donc il est indépendant de la
langue de Windows — les libellés, eux, sont traduits.

### Identité visuelle

Générée par `assets/build-assets.mjs` : la marque est un disque céladon avec la
voix creusée en négatif et le bruit tenu dehors en éclats ambre. Trois découpes,
parce qu'un seul dessin ne peut pas servir 16 px et 512 px, et deux jeux
d'icônes — produit (tuile sombre) et tray (transparent, il se pose sur une barre
des tâches claire ou sombre). Le logotype est de la géométrie, sans dépendance à
une police installée : GitHub rastérise les SVG sous Linux.

## 13. ✅ Chaîne de distribution validée (2026-08-04)

Premier test réel de bout en bout : release publique GitHub → téléchargement →
installation sur une machine **purgée de tout certificat Kwiet**.

Deux échecs préalables, tous deux nommés par Windows, qui identifient la cause
sans ambiguïté :

| Code | Signification | Cas mesuré |
|---|---|---|
| `0xE000026F` | pas de catalogue | release non signée : `build-package.ps1` sans certificat s'arrête avant `makecat` |
| `0x800B0109` | `CERT_E_UNTRUSTEDROOT` | paquet signé, certificat absent des magasins de la machine |

Ce que fait désormais l'installeur, dans cet ordre :

```
Certificat de signature approuve (CN=Kwiet Project, …, D6B261AA…)
pnputil /add-driver kwiet_component.inf /install
pnputil /add-driver kwiet_extension.inf /install
Pack installe. Packages publies : oem176.inf, oem83.inf
```

Le certificat **public** voyage dans le pack et est extrait du PFX au moment de
signer, donc il ne peut pas désigner une autre clé que celle qui a signé. La clé
privée ne quitte jamais la construction : elle vit dans un secret du dépôt. Le
certificat porte l'EKU *Code Signing* seul — il ne peut valider aucune chaîne
TLS. La désinstallation le retire des magasins.

Ce que ça ne règle pas : c'est toujours un octroi de confiance réel sur la
machine de l'utilisateur. Un certificat de signature de code commercial chaîne
vers une racine déjà présente partout et ne demande d'ajouter rien du tout ;
c'est la seule voie qui évite complètement ce compromis.

## 14. ⛔ Chromium contourne les packs d'effets — Firefox non (2026-08-04)

C'est la limite fondamentale du produit, et elle n'est pas dans notre code.

### Mesures, même machine, même micro, même heure

| Consommateur | Instanciation de l'APO | `LockForProcess` | Verdict |
|---|---|---|---|
| ffmpeg (WASAPI partagé) | oui | oui | **traite** |
| Enregistreur vocal, vumètre des Paramètres | oui | oui | **traite** |
| **Firefox** `getUserMedia` | oui | 18:04:55 | **traite** — VU −38 dB → −76 dB |
| **Chrome / Edge** `getUserMedia` | **aucune** | non | contourné |

Le cas Chrome a été testé dans quatre configurations, toutes à zéro ligne de
journal :

1. contraintes par défaut (`echoCancellation`/`noiseSuppression`/`autoGainControl` à `true`) ;
2. les trois contraintes à `false` ;
3. `--disable-features=WASAPIRawAudioCapture` ;
4. avec `IApoAcousticEchoCancellation` déclarée par l'APO.

Aucune ne change quoi que ce soit. Chromium n'instancie jamais l'APO.

### Pourquoi

Chromium fait tourner son propre pipeline WebRTC (AEC3, suppression de bruit,
AGC) et demande à Windows de l'audio non traité pour que les deux ne se
superposent pas. La définition Microsoft d'un flux RAW le dit :
*« bypasses all signal processing except for endpoint specific, **always-on**
processing in the APO »*. Un pack d'effets sélectionnable n'est pas always-on :
il est donc écarté.

Ce n'est pas dirigé contre nous. **Voice Clarity, le pack de Microsoft, subit le
même sort** — c'est le même mécanisme, le même type de marqueur d'endpoint.
Aucun pack d'effets n'atteint un onglet Chrome.

### Ce que ça implique pour le produit

La promesse d'origine — « nettoyé pour **toutes** les applications » — ne tient
pas telle quelle. Le périmètre réel :

- ✅ applications natives : Discord natif, Zoom, Teams natif, OBS, jeux, tout
  enregistreur ;
- ✅ **Firefox**, et vraisemblablement tout moteur Gecko ;
- ⛔ Chrome, Edge, Brave, Vivaldi, et toute application Electron — donc Meet,
  Teams web, Slack, Discord *desktop* (Electron).

Et c'est là qu'on comprend rétrospectivement pourquoi la concurrence fait ce
qu'elle fait : **Krisp et NVIDIA Broadcast créent un micro virtuel**. Ce choix,
qu'on a écarté au premier jour comme inélégant, est exactement ce qui leur permet
de fonctionner dans Chrome — un périphérique virtuel est un périphérique, et
Chromium le capture comme n'importe quel autre. Notre approche est plus propre
pour l'utilisateur et structurellement aveugle à Chromium.

### Ce que ça ne remet pas en cause

L'APO fonctionne. Le DSP fonctionne. La chaîne d'installation fonctionne. La
limite est en amont de nous, dans la décision d'un navigateur, et elle
s'applique identiquement à tous nos concurrents non-virtuels.

### Piège de méthode, encore le même

Pendant des heures, la conclusion « ça marche dans Meet » a reposé sur
l'animation de niveau de Meet. Cet indicateur bougeait — mais il mesurait la
prévisualisation du sélecteur de micro (qui, elle, passe par l'APO), pas l'audio
de l'appel. Troisième occurrence du même défaut : **seul le journal de l'APO et
le bloc de contrôle prouvent quelque chose.** Tout indicateur d'application est
une hypothèse déguisée.

## 15. 🔓 Chromium n'est pas verrouillé — il est conditionnel (2026-08-04)

Le §14 concluait que Chromium contourne les packs d'effets. C'est vrai **par
défaut**, et faux dans l'absolu. La lecture du source l'établit.

### Ce que fait Chromium, dans son code

`media/audio/win/audio_low_latency_input_win.cc`,
`SetCommunicationsCategoryAndMaybeRawCaptureMode` :

```cpp
audio_props.eCategory = AudioCategory_Communications;
constexpr int kMaxRawCaptureChannels = 8;
if (channels > 0 && channels <= kMaxRawCaptureChannels) {
  audio_props.Options = AUDCLNT_STREAMOPTIONS_RAW;
}
// Use AUDCLNT_STREAMOPTIONS_NONE instead of AUDCLNT_STREAMOPTIONS_RAW if
// system AEC has been enabled to ensure that "Voice Clarity" can kick in.
if (aec_config_) {
  audio_props.Options = AUDCLNT_STREAMOPTIONS_NONE;
}
```

`aec_config_` vient de `EchoCancellationConfig::Create(params, …)`, non nul
seulement si `params.effects() & AudioParameters::ECHO_CANCELLER`.

**Donc : dès que l'AEC système est demandée, Chromium abandonne le mode RAW et
les effets système s'appliquent.** Le commentaire nomme même Voice Clarity.

### Le levier utilisateur

`chrome://flags` → **« Enforce system Echo Cancellation »**
(`--enable-features=EnforceSystemEchoCancellation`). Windows 11 24H2
(build 26100) minimum. Désactivé par défaut, en cours de déploiement.

Pistes mortes, testées et écartées :

- `echoCancellationType: 'system'` — la contrainte **n'existe plus**
  (`getSupportedConstraints().echoCancellationType === false`) ;
- `--enable-blink-features=ExperimentalHardwareEchoCancellation` — sans effet,
  la fonctionnalité expérimentale a été retirée ;
- `voiceIsolation` — exposée mais non implémentée sur Windows (ChromeOS d'abord) ;
- `--disable-features=WASAPIRawAudioCapture` — sans effet observable.

### Où ça bloque maintenant

Avec le flag actif, Chrome nous fait **entrer dans le graphe** :

```
Initialize: SE3, mode=COMMUNICATIONS, discoveryOnly=0     x111
QI ok: IApoAcousticEchoCancellation                       x166
LockForProcess: S_OK, 2->2 ch, 48000 Hz, latency=300000   x110
AddAuxiliaryInput                                          x55
UnlockForProcess … refFrames=0                            2 à 12 ms plus tard
```

Windows nous accepte comme annuleur d'écho et câble le flux de référence, puis
relâche avant qu'un échantillon ne circule, en boucle. Aucun `refFrames` non nul :
le verrouillage n'aboutit jamais à du traitement.

### Expérience finale, tous leviers actionnés ensemble (2026-08-04)

Magasin de pilotes purgé, un seul paquet, pack déclarant l'AEC, journal remis à
zéro, et Chrome lancé avec `EnforceSystemEchoCancellation`,
`WASAPIRawAudioCapture` désactivé et `ExperimentalHardwareEchoCancellation`.

Résultat : **inchangé**. 65 `LockForProcess` réussis, tous relâchés en 3 ms avec
`refFrames=0`. Le seul verrouillage long du journal — 15,3 secondes — était le
vumètre des Paramètres Windows pendant la re-sélection du pack, confirmé par le
propriétaire de la machine, et non Chrome.

Windows nous accepte comme annuleur d'écho, câble le flux de référence, puis
nous sort avant qu'un échantillon ne circule. La lecture la plus simple, et
désormais la mieux étayée : **on annonce un annuleur d'écho qui n'annule rien**,
et le moteur s'en aperçoit.

Hypothèses pour la suite, aucune mesurée :

1. **Le contrat AEC n'est pas honoré.** On déclare l'interface sans annuler quoi
   que ce soit — `AcceptInput` se contente de compter les trames de référence.
   Le moteur valide peut-être davantage.
2. **La latence annoncée.** 300000 hns = 30 ms. Le pipe communications sert
   l'annulation d'écho, qui exige un alignement serré entre référence et micro.
3. **Le nombre de canaux.** Chrome demande `channelCount: 1` ; on annonce
   2 → 2. `IAudioProcessingObjectPreferredFormatSupport` n'est toujours pas
   implémentée, et le commentaire du code prévoyait déjà « une version future
   qui répond correctement avec un format mono ».
4. **`{69E1F79F-6EAE-4517-BE9F-13AA90E30014}`**, réclamée 221 fois et jamais
   identifiée depuis le jalon 1. Toujours la seule inconnue franche du dossier.

### Ce que ça change pour le produit

Le blocage Chromium n'est pas structurel. Il tient à un drapeau désactivé par
défaut, que Google déploie, et à un contrat d'annuleur d'écho qu'il faudrait
honorer pour de bon. La perspective réaliste : **Kwiet dans Chrome exigerait
d'implémenter réellement l'annulation d'écho**, ce qui était précisément le
chantier écarté au §11bis.

## 16. ✅ Annulation d'écho intégrée — AEC3 en Rust pur (2026-08-04)

Décision inversée, sur mesure et non sur argument. Le §11bis écartait
l'annulation d'écho parce qu'on n'aurait livré que du SpeexDSP médiocre face à
AEC3. Le paysage a changé : [`sonora`](https://github.com/dignifiedquire/sonora)
est **AEC3 lui-même, porté en Rust pur**, BSD-3-Clause.

| | |
|---|---|
| Compilation MSVC | 11 s, aucune adaptation, aucune dépendance C++ |
| Validation amont | 2400+ tests de la suite de référence WebRTC |
| Coût mesuré ici | **72 µs** par trame de 10 ms — RTF 0,0072 |
| Annulation mesurée | **74,3 dB** via notre propre ABI C (écho à −4 dB, retard 2,5 ms) |
| RTF total avec DFN3 | **0,027** — inchangé en pratique |

### Chaîne

```
AcceptInput (RT)  →  anneau SPSC mono  →  worker
                                            ├─ AEC3      (écho, corrélé)
                                            └─ DFN3      (bruit, non corrélé)
```

L'ordre n'est pas indifférent : l'écho est une copie corrélée d'un signal qu'on
nous donne, qu'un filtre adaptatif retire proprement. Le laisser au débruiteur
reviendrait à lui demander d'effacer de la parole.

Quand la référence manque — rien ne joue, ou pas d'entrée auxiliaire câblée — le
worker nourrit le canceller de silence plutôt que de sauter le bloc, pour que sa
notion du temps reste alignée sur la capture.

### Conséquence sur le contrat

`IApoAcousticEchoCancellation` est **désormais déclarée**, et honorée. Elle avait
été délibérément retenue tant que l'annulation n'existait pas : un APO qui
réclame ce marqueur fait couper aux applications leur propre annuleur.

Validé en non-régression : capture normale, `refFrames=175680` (3,7 s de
référence réellement reçue), 0 décrochage, 0 erreur.

### Ce que ça ne débloque pas

Chrome. Testé avec l'AEC réelle et le flag `EnforceSystemEchoCancellation` :
rejet identique, verrouillages de 2 à 4 ms. L'hypothèse « on ment sur l'AEC »
est donc écartée — Windows ne peut de toute façon pas mesurer la qualité d'une
annulation en 3 ms.

Écartée aussi, par expérience dédiée : **la latence annoncée**. Reporter zéro
hns au lieu de 300000 n'a rien changé sur 28 verrouillages.

Restent, non testées : le format (on verrouille en `2->2 ch` quand Chrome
demande du mono, et `IAudioProcessingObjectPreferredFormatSupport` n'est
toujours pas implémentée), et `{69E1F79F-6EAE-4517-BE9F-13AA90E30014}`,
réclamée 221 fois, refusée 221 fois, absente du registre, du SDK et du web.

### Confirmation par l'usage

Le propriétaire de la machine, sans instrumentation : dans WhatsApp — application
Windows **native** depuis sa réécriture — un bébé qui hurle derrière lui est
inaudible pour son interlocuteur ; dans Meet, tout passe. Même casque, même
micro, même instant. C'est la meilleure démonstration de la session, et elle
établit aussi que **Voice Clarity ne fonctionne pas davantage dans Chrome**.

## 17. Identifier les interfaces refusées — méthode et résultats (2026-08-04)

Windows réclamait sept interfaces qu'on refusait, toutes inconnues. Aucune n'est
dans le registre, aucune n'est dans le SDK 19041 installé, aucune n'est
indexée sur le web. La méthode qui a marché :

1. **Chercher le GUID sous forme binaire** dans les modules audio de Windows.
   Un GUID en mémoire est little-endian sur ses trois premiers champs : une
   recherche textuelle passe à côté. `{69E1F79F-…}` apparaît dans `AudioEng.dll`,
   `AudioSes.dll`, `audiosrv.dll` et **`VirtualSurroundApo.dll`** — ce dernier
   étant un APO Microsoft, donc quelque chose qui *implémente* l'interface.
2. **Lire les GUID voisins.** Dans une table `.rdata` ils sont contigus. Celui
   qu'on cherchait est encadré par `IAudioProcessingObject` et
   `IAudioProcessingObjectRT` : c'est bien la table de QueryInterface d'un APO,
   et l'inconnue appartient à la même famille.
3. **Regrepper les en-têtes vendorés** avec les voisines. Deux tombent :

| GUID | Interface |
|---|---|
| `{51CBD3C4-F1F3-4D2F-A0E1-7E9C4DD0FEB3}` | `IAudioProcessingObjectPreferredFormatSupport` |
| `{CA2CFBDE-A9D6-4EB0-BC95-C4D026B380F0}` | `IAudioProcessingObjectNotifications2` |

### `IAudioProcessingObjectPreferredFormatSupport` — implémentée, en mono

Le §7bis la déclarait volontairement absente, sur l'observation que répondre
`E_NOTIMPL` laissait le moteur bloqué. La bonne réponse n'était ni de la retirer
ni de répondre « je ne sais pas », mais de **dire le format dans lequel le
moteur travaille réellement** : AEC3 et DeepFilterNet3 sont mono tous les deux,
les canaux sont sommés à l'entrée et le résultat réétalé.

Résultat mesuré : le verrouillage passe de `2->2 ch` à **`2->1 ch`**. La
négociation change donc bien, et `GetPreferredOutputFormat` est appelée et
honorée. Non-régression vérifiée sur capture normale : 0 décrochage, 0 erreur.

`CreateAudioMediaTypeFromUncompressedAudioFormat` vit dans
`audiomediatypecrt.lib`, qui traîne une dépendance ATL absente de cette
installation. `IAudioMediaType` compte quatre méthodes : `MonoMediaType.h`
l'implémente en quarante lignes, ce qui évite d'imposer un composant Visual
Studio entier au projet.

### Ce que ça ne débloque toujours pas

Chrome. Verrouillage relâché après 3 ms, comme avant. Restent refusées, par
fréquence : `{69E1F79F-…}` (393 fois, la seule inconnue franche qui reste),
`{F235855F-…}` (`IApoAcousticEchoCancellation2`), `{B1176E34-…}`
(`IAudioSystemEffectsCustomFormats`), `{CA2CFBDE-…}` (`…Notifications2`).

Les deux dernières sont désormais nommées et implémentables.

### `{69E1F79F-…}` identifiée : `IAudioProcessingObjectInternal`

La méthode, en trois temps, sans désassembleur :

1. **Extraire la signature PDB du binaire.** Le répertoire de debug d'un PE
   contient un enregistrement CodeView `RSDS` : GUID, âge, nom du PDB. De quoi
   construire l'URL du symbol server de Microsoft —
   `https://msdl.microsoft.com/download/symbols/<pdb>/<GUID><âge>/<pdb>`.
2. **Télécharger le PDB.** `VirtualSurroundApo.pdb`, 0,34 Mo, publiquement
   servi.
3. **En extraire les noms de types**, qui y figurent en clair.

Le PDB nomme exactement six interfaces de la famille APO :
`IAudioProcessingObject`, `…Configuration`, `…RT`, `IAudioSystemEffects`,
`…2`, et **`IAudioProcessingObjectInternal`**. Les cinq premières ont un GUID
connu ; dans la table QI du binaire, l'inconnue siège entre
`IAudioProcessingObject` et `…RT`. Il ne reste qu'un nom pour un GUID.

> Déduction par élimination, solide mais non prouvée formellement. Le PDB ne
> relie pas explicitement le GUID au nom.

**Ce que le nom implique.** *Internal* : interface interne Microsoft, absente du
SDK, du registre et du web parce qu'elle n'est pas destinée aux tiers. On n'en
connaît ni les méthodes, ni le contrat, ni la sémantique, et Windows la réclame
à chaque instanciation — 393 fois sur une seule session de test.

⚠️ **Portée à ne pas surestimer.** Il serait tentant d'en conclure qu'aucun APO
tiers n'est admis. **Firefox contredit cela** : son `getUserMedia` fait
verrouiller et traiter cet APO sans difficulté. Windows n'exclut donc pas les
APO tiers, et cette interface n'est pas un péage général.

Ce que les mesures disent, précisément :

| Mode du pipe | Consommateur | Résultat |
|---|---|---|
| DEFAULT | Firefox, ffmpeg, Enregistreur vocal | verrouillé, traite |
| COMMUNICATIONS | Chrome | verrouillé puis lâché en 3 ms |

Et c'est **Chrome qui choisit ce pipe** : son code pose
`eCategory = AudioCategory_Communications` et le conserve même quand
`EnforceSystemEchoCancellation` désactive le mode RAW. Firefox ne le fait pas.

L'hypothèse tenable est donc plus étroite : `IAudioProcessingObjectInternal`
serait requise **dans le pipe communications**, pas pour un APO tiers en
général. Elle reste une hypothèse.

### L'expérience qui trancherait

Sélectionner **Voice Clarity** au lieu de Kwiet, lancer Chrome avec le flag, et
mesurer si la suppression de bruit s'applique. Voice Clarity est un pack
d'effets Microsoft : il implémente vraisemblablement l'interface interne.

- Si Voice Clarity fonctionne dans Chrome et pas nous → le pipe communications
  réserve l'accès aux APO Microsoft, et c'est un mur.
- Si Voice Clarity échoue aussi → le pipe communications n'accepte aucun pack
  d'effets, l'interface interne n'y est pour rien, et le problème est ailleurs.

## 8. Questions ouvertes

- **`APOInitSystemEffects3`** (Win11 22H2+) : layout différent de SE2 — le
  shim ne lit le mode de traitement que sur une taille SE2 exacte ; à traiter
  proprement au jalon 2 (le passthrough n'en a pas besoin).
- **Cohabitation avec des FX driver existants** (`PKEY_CompositeFX_*`) :
  non gérée au jalon 1 — install.ps1 refuse d'écraser un APO existant sans
  `-Force`. À adresser avant toute installation sur du vrai matériel
  (Realtek/Intel Smart Sound ont souvent leurs propres APO capture).
- **44,1 kHz** : DFN3 attend du 48 kHz ; resampling dans le worker ou refus du
  format — à trancher au jalon 2.
- Windows peut couper les « améliorations audio » d'un endpoint après des
  crashs répétés d'audiodg — comportement à caractériser pendant le soak test.
