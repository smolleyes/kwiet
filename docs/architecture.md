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
| COM | implémentation directe sans ATL ; `ThreadingModel=Both` ; enregistrement sous `HKLM\Software\Classes\CLSID` |
| Flags APO | `APO_FLAG_DEFAULT` (formats identiques in/out imposés par l'engine, vérifiés quand même à `LockForProcess`) |
| Formats | float32 interleaved uniquement (`KSDATAFORMAT_SUBTYPE_IEEE_FLOAT`) |
| `GetEffectsList` | vide au jalon 1 ; exposera `KWIET_EFFECT_NoiseSuppression` au jalon 2 |
| `GetLatency` | 0 au jalon 1 ; retard fixe du ring au jalon 2 |
| DLL | `C:\Program Files\Kwiet\KwietApo.dll` (lisible par le service audio) |
| APO non signé | `HKLM\...\CurrentVersion\Audio\DisableProtectedAudioDG = 1` en dev, retiré à la désinstallation ; disparaîtra avec la signature SignPath |

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
