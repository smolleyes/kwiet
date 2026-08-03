#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>

// Shared-memory control plane between the APO (inside audiodg, session 0) and
// the Kwiet UI (interactive session).
//
// THIS HEADER IS THE CONTRACT. Any change to the layout must bump
// KWIET_CONTROL_VERSION and be mirrored in the UI.
//
// Direction of each field is documented below. Everything is a 32-bit atomic:
// no locks, no pointers and no sizes ever cross the boundary, so a hostile
// writer can at worst set nonsense values, which readers clamp.

#define KWIET_CONTROL_NAME    L"Global\\KwietControlV1"
#define KWIET_CONTROL_MAGIC   0x5449574Bu // 'KWIT' little-endian
#define KWIET_CONTROL_VERSION 1u

// Aggressiveness is carried in tenths of a dB to keep the block integer-only.
#define KWIET_AGGRESSIVENESS_MIN_TENTHS 0
#define KWIET_AGGRESSIVENESS_MAX_TENTHS 1000
// 50 dB : compromis retenu à l'écoute. Le maximum (100 dB) descend le bruit
// jusqu'au silence numérique, ce qui rabote les attaques et fait légèrement
// pomper ; un plancher résiduel masque ces artefacts.
#define KWIET_AGGRESSIVENESS_DEFAULT_TENTHS 500

// Peak levels are linear amplitude scaled to 0..32767, which keeps the block
// integer-only and is plenty for a VU meter.
#define KWIET_PEAK_SCALE 32767

#pragma pack(push, 4)
struct KwietControlBlock
{
    // --- identity, written once by the APO when it creates the block ---
    std::atomic<uint32_t> magic;
    std::atomic<uint32_t> version;

    // --- UI -> APO ---
    std::atomic<int32_t> enabled;               // 0 = bypass, non-zero = on
    std::atomic<int32_t> aggressivenessTenths;  // clamped to the range above

    // --- APO -> UI ---
    std::atomic<int32_t> streaming;    // 1 between LockForProcess/UnlockForProcess
    std::atomic<int32_t> generation;   // incremented per stream, lets the UI
                                       // notice a restart and re-push settings
    std::atomic<int32_t> sampleRate;
    std::atomic<int32_t> channels;
    std::atomic<int32_t> latencyFrames;
    std::atomic<int32_t> peakIn;       // 0..KWIET_PEAK_SCALE, pre-DSP
    std::atomic<int32_t> peakOut;      // 0..KWIET_PEAK_SCALE, post-DSP
    std::atomic<int32_t> underruns;
    std::atomic<int32_t> dspErrors;
    std::atomic<int32_t> dspActive;    // 1 when the DSP pipeline is running,
                                       // 0 when the APO is a plain passthrough

    int32_t reserved[16];              // room to grow without a version bump
};
#pragma pack(pop)

static_assert(sizeof(std::atomic<int32_t>) == sizeof(int32_t),
              "the control block must stay layout-compatible across processes");
static_assert(std::atomic<int32_t>::is_always_lock_free,
              "cross-process atomics must be lock-free");
