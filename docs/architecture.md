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

### État de validation (2026-08-03)

`installer/effectpack/sign-install-dev.ps1` installe le pack sans erreur :
device `SWD\DRIVERENUM\{...}#KWIETEFFECTPACK` présent et **OK**, composant
enregistré en `...\Class\{5989fce8-…}\0005` avec les trois sous-clés
(`AudioEngine`, `Classes`, `EffectPackRegistration`) — structure identique à
Voice Clarity en `0001`. **Mais** aucun endpoint ne référence encore le CLSID
Kwiet et la DLL n'est pas chargée. Reste à tester, dans l'ordre :

1. **redémarrage complet** (AudioEndpointBuilder n'évalue probablement les
   packs qu'à la construction des endpoints — un `pnputil /scan-devices` +
   restart de service n'a pas suffi) ;
2. si insuffisant : comparer finement notre `EffectPackRegistration` à celle de
   Voice Clarity (`reg export` des deux sous-arbres, diff) ;
3. vérifier si le moteur exige une signature d'éditeur particulière pour
   *appliquer* un pack (au-delà de l'acceptation du package par pnputil).

> ⚠️ Effet de bord constaté : le marqueur `{9E6136E0-…},100` de Voice Clarity a
> disparu de l'endpoint casque après nos écritures manuelles de FxProperties.
> Un redémarrage devrait le faire réécrire par AudioEndpointBuilder ; à
> vérifier explicitement.

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
