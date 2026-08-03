# dsp/ — moteur DSP Rust exposé en ABI C

Crate `cdylib` chargé par l'APO depuis le DriverStore. **Jalon 2 : effet
trivial (atténuation fixe, −6 dB par défaut)**, destiné à valider la
plomberie temps-réel avant d'y brancher DeepFilterNet3 (crate
[`df`](https://github.com/Rikorose/DeepFilterNet), dual MIT/Apache-2.0).

```powershell
cargo test --release                              # 11 tests
cargo clippy --release --all-targets -- -D warnings
cargo build --release                             # -> target/release/kwiet_dsp.dll
```

Points non négociables (détail et raisons dans
[`../docs/architecture.md`](../docs/architecture.md) §9) :

- **CRT statique** via `.cargo/config.toml` — sans ça la DLL importe
  `VCRUNTIME140.dll` et peut ne pas se charger dans `audiodg`.
- **Aucun panic ne traverse la frontière C** : `catch_unwind` à chaque point
  d'entrée, et surtout pas `panic = "abort"` (un abort tuerait `audiodg`,
  donc tout le son de la machine).
- `kwiet_dsp_process` **n'alloue pas** et ne bloque pas.

ABI stable, déclarée dans [`include/kwiet_dsp.h`](include/kwiet_dsp.h) :

```c
uint32_t  kwiet_dsp_abi_version(void);                              // garde anti-DLL périmée
KwietDsp* kwiet_dsp_create(uint32_t sample_rate, uint32_t channels); // hors RT
void      kwiet_dsp_destroy(KwietDsp*);
int32_t   kwiet_dsp_process(KwietDsp*, const float* in, float* out, uint32_t frames);
void      kwiet_dsp_set_attenuation_db(KwietDsp*, float db);        // thread-safe, atomic
```

## Étape suivante : DeepFilterNet3

Remplacer l'atténuation par un wrapper de la crate `df`, en gardant l'ABI
inchangée. Points à trancher à ce moment-là :

- vérifier que `process` **n'alloue pas après warmup** (préchauffer, ou
  wrapper si la crate alloue) — c'est la contrainte structurante ;
- DFN3 attend du **48 kHz mono** : gérer le resampling et le downmix depuis le
  format négocié (`sample_rate()` est déjà conservé pour ça) ;
- le lookahead de DFN3 s'ajoute aux 30 ms du ring — vérifier qu'on reste dans
  le budget 40-60 ms.
