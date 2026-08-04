# dsp/ — the Rust noise-suppression engine, behind a C ABI

A `cdylib` loaded by the APO from the driver store. It wraps
[DeepFilterNet3](https://github.com/Rikorose/DeepFilterNet) (crate `deep_filter`,
dual MIT/Apache-2.0) with the model embedded in the DLL.

```powershell
cargo test --release
cargo clippy --release --all-targets -- -D warnings
cargo build --release        # -> target/release/kwiet_dsp.dll
```

Non-negotiable, with the reasoning in
[`../docs/architecture.md`](../docs/architecture.md) §9:

- **Static CRT** via `.cargo/config.toml`. Without it the DLL imports
  `VCRUNTIME140.dll` and may fail to load inside `audiodg`.
- **No panic crosses the C boundary**: `catch_unwind` at every entry point, and
  emphatically not `panic = "abort"` — an abort would kill `audiodg`, and with
  it every sound on the machine.
- `kwiet_dsp_process` is called from the worker thread, never the real-time one.

## ABI

Declared in [`include/kwiet_dsp.h`](include/kwiet_dsp.h):

```c
uint32_t  kwiet_dsp_abi_version(void);                               // guards against a stale DLL
KwietDsp* kwiet_dsp_create(uint32_t sample_rate, uint32_t channels); // not real-time
void      kwiet_dsp_destroy(KwietDsp*);
uint32_t  kwiet_dsp_block_frames(void);                              // fixed frame the host must feed
int32_t   kwiet_dsp_process(KwietDsp*, const float* in, float* out, uint32_t frames);
void      kwiet_dsp_set_attenuation_db(KwietDsp*, float db);         // thread-safe, atomic
```

## Dependency pins, and why they are mandatory

The crate published on crates.io (`deep_filter` 0.2.5, 2022) is still DFN2, so we
depend on the Git repository. Two pins are needed to compile at all, both
commented in `Cargo.toml`:

- **the `tract` family at `=0.21.4`** — `deep_filter` asks for `^0.21.4`, but
  `tract` renamed a public field (`symbol_table` → `symbols`) in a release cargo
  considers compatible. The pin must live **in the manifest**:
  `cargo update --precise` downgrades crate by crate and breaks the family's
  internal coherence (`tract-pulse-opl` requires `=` its own version);
- **`kstring` at `2.0.2`** — 2.0.4 requires rustc 1.96.

The model ships inside the DLL: `DfParams::default()` embeds
`DeepFilterNet3_onnx.tar.gz` (7.6 MB) through the `default-model` feature. There
is no file to deploy next to it.

## DeepFilterNet3 constraints

- **48 kHz only.** `kwiet_dsp_create` returns `NULL` for any other rate and the
  host stays in passthrough. Resampling is still to be done.
- **Mono.** The host's channels are summed on the way in, and the mono result is
  written back across every channel.
- **Fixed frame** of `kwiet_dsp_block_frames()` (480 = 10 ms). The host uses that
  as its worker's block size; the rings decouple it from the APO's quantum.
- `process` **allocates** — tract allocates per inference. That is acceptable
  precisely because it runs on the worker and never on the real-time thread.
