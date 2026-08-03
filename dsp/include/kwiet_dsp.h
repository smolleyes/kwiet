#ifndef KWIET_DSP_H
#define KWIET_DSP_H

// Stable C ABI between apo/ (C++, host) and dsp/ (Rust cdylib).
// Keep this header and dsp/src/lib.rs in sync; the ABI version guards against
// loading a stale DLL from the DriverStore.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// v2: DeepFilterNet3 replaced the placeholder gain. This changed the meaning
// of kwiet_dsp_set_attenuation_db and added kwiet_dsp_block_frames.
#define KWIET_DSP_ABI_VERSION 2u

// Return codes of kwiet_dsp_process. Anything < 0 makes the host fail open
// (passthrough) for that block.
#define KWIET_DSP_OK             0
#define KWIET_DSP_ERR_HANDLE   (-1)
#define KWIET_DSP_ERR_ARGS     (-2)
#define KWIET_DSP_ERR_INTERNAL (-3)

typedef struct KwietDsp KwietDsp;

// ABI version of the loaded library; compare against KWIET_DSP_ABI_VERSION
// before using any other entry point.
uint32_t kwiet_dsp_abi_version(void);

// Creates an engine. NOT real-time safe: loads and optimises the network.
// Returns NULL when the format is unsupported -- notably any rate other than
// 48000 Hz, which DeepFilterNet3 does not handle -- or when the model fails to
// load. The host then stays in passthrough.
KwietDsp* kwiet_dsp_create(uint32_t sample_rate, uint32_t channels);

// Destroys an engine. NULL is accepted and ignored.
void kwiet_dsp_destroy(KwietDsp* dsp);

// Frames consumed per inference (480 = 10 ms at 48 kHz). kwiet_dsp_process
// requires a non-zero multiple of this; the host uses it as its worker block
// size. Returns 0 for a NULL handle.
uint32_t kwiet_dsp_block_frames(const KwietDsp* dsp);

// Processes `frames` interleaved float samples (frames * channels values),
// `frames` being a non-zero multiple of kwiet_dsp_block_frames().
// `in` and `out` may alias exactly (in == out) but must not partially overlap.
// Only one thread may be inside this function at a time.
// NOTE: runs on the host's worker thread, never on the audio thread, so it is
// allowed to allocate (tract does, per inference).
int32_t kwiet_dsp_process(KwietDsp* dsp, const float* in, float* out, uint32_t frames);

// Sets the MAXIMUM NOISE ATTENUATION in dB: 0 leaves the signal untouched,
// 100 lets DeepFilterNet suppress as much as it wants. Aggressiveness control,
// not a gain on the signal. Thread-safe (atomic), callable while process runs;
// the value takes effect on the next block. Out-of-range values are clamped.
void kwiet_dsp_set_attenuation_db(KwietDsp* dsp, float db);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // KWIET_DSP_H
