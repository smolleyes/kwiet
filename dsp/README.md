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

## Dépendance DeepFilterNet3 — épinglages obligatoires

Le crate publié sur crates.io (`deep_filter` 0.2.5, 2022) est encore DFN2 : on
dépend donc du dépôt Git. Deux épinglages sont nécessaires pour que ça
compile, tous deux commentés dans `Cargo.toml` :

- **famille `tract` en `=0.21.4`** — `deep_filter` déclare `^0.21.4`, mais
  `tract` a renommé un champ public (`symbol_table` → `symbols`) dans une
  version que cargo juge compatible. Résolu en 0.21.17, `deep_filter` ne
  compile plus. L'épinglage doit être fait **dans le manifeste** :
  `cargo update --precise` rétrograde crate par crate et casse la cohérence
  interne de la famille (`tract-pulse-opl` exige `=` sa propre version) ;
- **`kstring` en `2.0.2`** — la 2.0.4 exige rustc 1.96.

Modèle : `DfParams::default()` embarque `DeepFilterNet3_onnx.tar.gz` (7,6 Mo)
dans la DLL via la feature `default-model`. Aucun fichier à déployer à côté.

## Contraintes DFN3

- **48 kHz uniquement** : `kwiet_dsp_create` renvoie `NULL` pour tout autre
  taux, l'hôte reste alors en passthrough. Le resampling reste à faire.
- **Mono** : les canaux de l'hôte sont sommés en entrée, et le résultat mono
  est réécrit sur tous les canaux.
- **Trame fixe** de `kwiet_dsp_block_frames()` (480 = 10 ms) : l'hôte utilise
  cette valeur comme taille de bloc de son worker, les rings découplant ça du
  quantum de l'APO.
- `process` **alloue** (tract alloue par inférence). C'est acceptable parce
  qu'il tourne sur le worker, jamais sur le thread temps-réel.
