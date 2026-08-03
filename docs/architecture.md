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

## 6. Contrôle UI ↔ APO ✅

Shared memory nommée + atomics uniquement : enable/bypass, agressivité,
VU-mètres avant/après. L'UI ne parle jamais au thread RT. (Layout précis :
jalon 4.)

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
test signing (validé sur cette machine). La distribution passera par une
signature attestation Partner Center.

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
