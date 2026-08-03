#ifndef KWIET_DSP_H
#define KWIET_DSP_H

// Stable C ABI between apo/ (C++, host) and dsp/ (Rust cdylib).
// Keep this header and dsp/src/lib.rs in sync; the ABI version guards against
// loading a stale DLL from the DriverStore.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KWIET_DSP_ABI_VERSION 1u

// Return codes of kwiet_dsp_process. Anything < 0 makes the host fail open
// (passthrough) for that quantum.
#define KWIET_DSP_OK             0
#define KWIET_DSP_ERR_HANDLE   (-1)
#define KWIET_DSP_ERR_ARGS     (-2)
#define KWIET_DSP_ERR_INTERNAL (-3)

typedef struct KwietDsp KwietDsp;

// ABI version of the loaded library; compare against KWIET_DSP_ABI_VERSION
// before using any other entry point.
uint32_t kwiet_dsp_abi_version(void);

// Creates an engine. NOT real-time safe: allocates. Returns NULL on failure.
KwietDsp* kwiet_dsp_create(uint32_t sample_rate, uint32_t channels);

// Destroys an engine. NULL is accepted and ignored.
void kwiet_dsp_destroy(KwietDsp* dsp);

// Processes `frames` interleaved float samples (frames * channels values).
// `in` and `out` may alias exactly (in == out) for in-place processing, but
// must not partially overlap. Allocation-free after warmup.
int32_t kwiet_dsp_process(KwietDsp* dsp, const float* in, float* out, uint32_t frames);

// Thread-safe (atomic), callable while process() runs.
void kwiet_dsp_set_attenuation_db(KwietDsp* dsp, float db);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // KWIET_DSP_H
