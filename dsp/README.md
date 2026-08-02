# dsp/ — cdylib Rust DeepFilterNet3 (jalon 2)

**Ne rien implémenter ici tant que le jalon 1 (APO passthrough stable 48 h en
VM) n'est pas validé.**

Contenu prévu : crate Rust `cdylib` wrappant la crate
[`df`](https://github.com/Rikorose/DeepFilterNet) (DeepFilterNet3, dual
MIT/Apache-2.0), exposant l'ABI C stable définie dans
[`../docs/architecture.md`](../docs/architecture.md#5-abi-c-entre-apo-et-dsp) :

```c
typedef struct KwietDsp KwietDsp;
KwietDsp* kwiet_dsp_create(uint32_t sample_rate, uint32_t channels); // hors RT
void      kwiet_dsp_destroy(KwietDsp*);
int32_t   kwiet_dsp_process(KwietDsp*, const float* in, float* out, uint32_t frames);
void      kwiet_dsp_set_attenuation_db(KwietDsp*, float db); // thread-safe, atomic
```

Ordre d'intégration au jalon 2 : d'abord un DSP trivial (gain −6 dB) pour
valider rings + worker + ABI, puis DFN3. Vérifier que `process` n'alloue pas
après warmup (préchauffer ou wrapper la crate `df` sinon).
