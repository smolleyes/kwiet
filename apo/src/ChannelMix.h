#pragma once

#include <windows.h>

#include <cstring>

// Interleaved channel conversion, RT-safe: no allocation, no branching beyond
// the fast path, no syscall.
//
// The communications pipe feeds a mono encoder, so the engine may hand us a
// stereo microphone and ask for a mono output. Declaring
// APO_FLAG_SAMPLESPERFRAME_MUST_MATCH would forbid that -- and appears to be
// what kept the APO out of that pipe (docs/architecture.md §11).
//
// Any count-to-count conversion goes through mono: averaging in, spreading
// out. That is correct both for DSP output (all channels already carry the
// same enhanced mono signal, so the average is a no-op) and for bypassed
// audio (a genuine downmix rather than dropping channels).
inline void MixChannels(const float* src, UINT32 srcChannels, float* dst, UINT32 dstChannels,
                        UINT32 frames)
{
    if (src == nullptr || dst == nullptr || srcChannels == 0 || dstChannels == 0) {
        return;
    }
    if (srcChannels == dstChannels) {
        memcpy(dst, src, static_cast<size_t>(frames) * srcChannels * sizeof(float));
        return;
    }

    const float invSrc = 1.0f / static_cast<float>(srcChannels);
    for (UINT32 frame = 0; frame < frames; ++frame) {
        const float* in = src + static_cast<size_t>(frame) * srcChannels;
        float mono = 0.0f;
        for (UINT32 c = 0; c < srcChannels; ++c) {
            mono += in[c];
        }
        mono *= invSrc;

        float* out = dst + static_cast<size_t>(frame) * dstChannels;
        for (UINT32 c = 0; c < dstChannels; ++c) {
            out[c] = mono;
        }
    }
}
