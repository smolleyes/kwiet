#include "DspHost.h"

#include <cstdlib>
#include <cstring>

#include "ChannelMix.h"
#include "KwietDevLog.h"
#include "Module.h"

namespace {

// Quanta of processed audio kept ahead of the real-time thread. This is the
// pipeline's fixed delay and the slack the worker has to stay on time. Three
// 10 ms quanta leaves room for DeepFilterNet3's own lookahead within the
// 40-60 ms budget from docs/architecture.md.
constexpr UINT32 kLatencyQuanta = 3;

// Extra quanta of ring headroom on top of the latency, so a late worker wakeup
// does not immediately overrun the input ring.
constexpr UINT32 kHeadroomQuanta = 4;

// Worker poll period. The real-time thread must not signal an event (that is a
// syscall), so the worker polls instead. Well under the 10 ms quantum.
constexpr DWORD kWorkerPollMs = 2;

#if defined(KWIET_DEV_LOG)
// Dev-only override so the DSP can be A/B tested without rebuilding:
//   HKLM\SOFTWARE\Kwiet : AttenuationDbTenths (REG_DWORD, signed, tenths of dB)
// Since ABI v2 the value is DeepFilterNet's MAXIMUM NOISE ATTENUATION, not a
// gain: 0 = no suppression at all, 1000 (100 dB) = suppress freely. Absent
// means "leave the engine default" (100 dB).
// Read once per stream from LockForProcess, never from the RT path. The real
// control plane is the shared memory block of milestone 4.
bool ReadDevAttenuationDb(float* outDb)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Kwiet", 0,
                      KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD raw = 0;
    DWORD cb = sizeof(raw);
    DWORD type = 0;
    const LSTATUS st = RegQueryValueExW(key, L"AttenuationDbTenths", nullptr, &type,
                                        reinterpret_cast<BYTE*>(&raw), &cb);
    RegCloseKey(key);
    if (st != ERROR_SUCCESS || type != REG_DWORD) {
        return false;
    }
    *outDb = static_cast<float>(static_cast<INT32>(raw)) / 10.0f;
    return true;
}
#endif

} // namespace

bool DspHost::LoadLibraryFromModuleDir()
{
    // audiodg resolves our own DLL by filename through the system search path,
    // which does NOT include the DriverStore directory we live in. The
    // dependency therefore has to be loaded by absolute path, derived from
    // this module's own location.
    wchar_t path[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(g_kwietModule, path, ARRAYSIZE(path));
    if (len == 0 || len >= ARRAYSIZE(path)) {
        return false;
    }
    wchar_t* lastSlash = wcsrchr(path, L'\\');
    if (lastSlash == nullptr) {
        return false;
    }
    lastSlash[1] = L'\0';
    if (wcscat_s(path, L"kwiet_dsp.dll") != 0) {
        return false;
    }

    // ALTERED_SEARCH_PATH so the DSP's own dependencies resolve from its
    // directory too (DeepFilterNet3 will bring some).
    m_dspModule = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (m_dspModule == nullptr) {
        KWIET_LOG("DspHost: LoadLibrary failed, err=%lu", GetLastError());
        return false;
    }

    m_abiVersion = reinterpret_cast<AbiVersionFn>(
        reinterpret_cast<void*>(GetProcAddress(m_dspModule, "kwiet_dsp_abi_version")));
    m_create = reinterpret_cast<CreateFn>(
        reinterpret_cast<void*>(GetProcAddress(m_dspModule, "kwiet_dsp_create")));
    m_destroy = reinterpret_cast<DestroyFn>(
        reinterpret_cast<void*>(GetProcAddress(m_dspModule, "kwiet_dsp_destroy")));
    m_blockFramesFn = reinterpret_cast<BlockFramesFn>(
        reinterpret_cast<void*>(GetProcAddress(m_dspModule, "kwiet_dsp_block_frames")));
    m_process = reinterpret_cast<ProcessFn>(
        reinterpret_cast<void*>(GetProcAddress(m_dspModule, "kwiet_dsp_process")));
    m_processRender = reinterpret_cast<ProcessRenderFn>(
        reinterpret_cast<void*>(GetProcAddress(m_dspModule, "kwiet_dsp_process_render")));
    m_setAttenuation = reinterpret_cast<SetAttenuationFn>(
        reinterpret_cast<void*>(GetProcAddress(m_dspModule, "kwiet_dsp_set_attenuation_db")));

    if (m_abiVersion == nullptr || m_create == nullptr || m_destroy == nullptr
        || m_blockFramesFn == nullptr || m_process == nullptr || m_processRender == nullptr
        || m_setAttenuation == nullptr) {
        KWIET_LOG("DspHost: missing export in kwiet_dsp.dll");
        return false;
    }
    const UINT32 abi = m_abiVersion();
    if (abi != KWIET_DSP_ABI_VERSION) {
        KWIET_LOG("DspHost: ABI mismatch, dll=%u expected=%u", abi, KWIET_DSP_ABI_VERSION);
        return false;
    }
    return true;
}

bool DspHost::Start(UINT32 sampleRate, UINT32 inChannels, UINT32 outChannels,
                    UINT32 framesPerQuantum, KwietControlBlock* control)
{
    Stop();

    if (sampleRate == 0 || inChannels == 0 || outChannels == 0 || framesPerQuantum == 0) {
        return false;
    }

    const UINT32 channels = inChannels;
    m_channels = inChannels;
    m_outChannels = outChannels;
    m_quantumFrames = framesPerQuantum;
    m_control = control;

    if (!LoadLibraryFromModuleDir()) {
        Stop();
        return false;
    }

    m_engine = m_create(sampleRate, channels);
    if (m_engine == nullptr) {
        // Most often an unsupported rate: DeepFilterNet3 is 48 kHz only.
        KWIET_LOG("DspHost: kwiet_dsp_create returned null (%u Hz, %u ch)", sampleRate, channels);
        Stop();
        return false;
    }

    // The DSP dictates its own block size (one inference), which the rings
    // decouple from the APO quantum -- that is exactly what they are for.
    m_blockFrames = m_blockFramesFn(m_engine);
    if (m_blockFrames == 0) {
        KWIET_LOG("DspHost: kwiet_dsp_block_frames returned 0");
        Stop();
        return false;
    }
    m_blockSamples = static_cast<size_t>(m_blockFrames) * channels;

    // Enough buffered audio for the RT thread to always find a quantum, and
    // for the worker to always find a whole block.
    const UINT32 byQuantum = framesPerQuantum * kLatencyQuanta;
    const UINT32 byBlock = m_blockFrames * 2;
    m_latencyFrames = byQuantum > byBlock ? byQuantum : byBlock;

    const size_t ringSamples =
        (static_cast<size_t>(m_latencyFrames) + framesPerQuantum * kHeadroomQuanta + m_blockFrames)
        * channels;
    if (!m_inRing.Init(ringSamples) || !m_outRing.Init(ringSamples)) {
        KWIET_LOG("DspHost: ring allocation failed");
        Stop();
        return false;
    }
    // Mono, so the same span of audio costs `channels` times less. Kept short
    // on purpose: a stale reference is worse than none, since the canceller
    // would subtract something that is no longer there.
    if (!m_renderRing.Init(ringSamples / channels)) {
        KWIET_LOG("DspHost: render ring allocation failed");
        Stop();
        return false;
    }

    m_scratchIn = static_cast<float*>(calloc(m_blockSamples, sizeof(float)));
    m_scratchOut = static_cast<float*>(calloc(m_blockSamples, sizeof(float)));
    m_renderScratch = static_cast<float*>(calloc(m_blockFrames, sizeof(float)));
    m_renderMix = static_cast<float*>(calloc(framesPerQuantum, sizeof(float)));
    // Sized on the quantum, not the block: this is what the RT thread pops.
    m_rtScratch = static_cast<float*>(
        calloc(static_cast<size_t>(framesPerQuantum) * channels, sizeof(float)));
    if (m_scratchIn == nullptr || m_scratchOut == nullptr || m_rtScratch == nullptr
        || m_renderScratch == nullptr || m_renderMix == nullptr) {
        KWIET_LOG("DspHost: scratch allocation failed");
        Stop();
        return false;
    }

    // Prime the output ring: this establishes the fixed delay and gives the
    // worker its slack. The stream therefore opens with kLatencyQuanta of
    // silence, which is the pipeline's algorithmic delay reported by
    // GetLatency -- not a failure mode.
    if (!m_outRing.PushZeros(static_cast<size_t>(m_latencyFrames) * channels)) {
        KWIET_LOG("DspHost: priming failed");
        Stop();
        return false;
    }

    m_stopEvent = CreateEventW(nullptr, TRUE /*manual reset*/, FALSE, nullptr);
    if (m_stopEvent == nullptr) {
        KWIET_LOG("DspHost: CreateEvent failed, err=%lu", GetLastError());
        Stop();
        return false;
    }

    m_dspFailed.store(false, std::memory_order_relaxed);
    m_underruns.store(0, std::memory_order_relaxed);
    m_overruns.store(0, std::memory_order_relaxed);
    m_dspErrors.store(0, std::memory_order_relaxed);

    m_worker = CreateThread(nullptr, 0, &DspHost::WorkerThunk, this, 0, nullptr);
    if (m_worker == nullptr) {
        KWIET_LOG("DspHost: CreateThread failed, err=%lu", GetLastError());
        Stop();
        return false;
    }

    // Release: everything above must be visible to the RT thread before it
    // sees m_active.
    m_active.store(true, std::memory_order_release);
    KWIET_LOG("DspHost: started, %u Hz, %u->%u ch, quantum=%u, block=%u, latency=%u frames",
              sampleRate, m_channels, m_outChannels, m_quantumFrames, m_blockFrames,
              m_latencyFrames);

#if defined(KWIET_DEV_LOG)
    float devDb = 0.0f;
    if (ReadDevAttenuationDb(&devDb)) {
        // Seed the control block rather than the engine directly: the worker
        // treats the block as authoritative and would immediately override a
        // value written straight to the engine.
        if (m_control != nullptr) {
            auto tenths = static_cast<int32_t>(devDb * 10.0f);
            if (tenths < KWIET_AGGRESSIVENESS_MIN_TENTHS) tenths = KWIET_AGGRESSIVENESS_MIN_TENTHS;
            if (tenths > KWIET_AGGRESSIVENESS_MAX_TENTHS) tenths = KWIET_AGGRESSIVENESS_MAX_TENTHS;
            m_control->aggressivenessTenths.store(tenths, std::memory_order_relaxed);
        } else {
            SetAttenuationDb(devDb);
        }
        KWIET_LOG("DspHost: dev attenuation override = %.1f dB", static_cast<double>(devDb));
    }
#endif
    return true;
}

void DspHost::Stop()
{
    // Close the RT path first: ProcessRt returns false from here on and the
    // APO fails open while we tear down.
    m_active.store(false, std::memory_order_release);

    if (m_worker != nullptr) {
        if (m_stopEvent != nullptr) {
            SetEvent(m_stopEvent);
        }
        WaitForSingleObject(m_worker, 2000);
        CloseHandle(m_worker);
        m_worker = nullptr;
    }
    if (m_stopEvent != nullptr) {
        CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
    }

    if (m_engine != nullptr && m_destroy != nullptr) {
        m_destroy(m_engine);
    }
    m_engine = nullptr;

    m_inRing.Free();
    m_outRing.Free();
    m_renderRing.Free();

    free(m_scratchIn);
    m_scratchIn = nullptr;
    free(m_scratchOut);
    m_scratchOut = nullptr;
    free(m_rtScratch);
    m_rtScratch = nullptr;
    free(m_renderScratch);
    m_renderScratch = nullptr;
    free(m_renderMix);
    m_renderMix = nullptr;

    if (m_dspModule != nullptr) {
        FreeLibrary(m_dspModule);
        m_dspModule = nullptr;
    }
    m_abiVersion = nullptr;
    m_create = nullptr;
    m_destroy = nullptr;
    m_blockFramesFn = nullptr;
    m_process = nullptr;
    m_processRender = nullptr;
    m_setAttenuation = nullptr;

    m_control = nullptr;
    m_channels = 0;
    m_outChannels = 0;
    m_quantumFrames = 0;
    m_blockFrames = 0;
    m_blockSamples = 0;
    m_latencyFrames = 0;
}

void DspHost::SetAttenuationDb(float db)
{
    // Only valid while started; the engine pointer is stable between
    // Start() and Stop(), both of which run off the RT path.
    if (m_active.load(std::memory_order_acquire) && m_setAttenuation != nullptr) {
        m_setAttenuation(m_engine, db);
    }
}

bool DspHost::ProcessRt(const float* in, float* out, UINT32 frames)
{
    // REAL-TIME PATH. No allocation, no lock, no syscall, no logging.
    if (!m_active.load(std::memory_order_acquire)) {
        return false;
    }
    if (in == nullptr || out == nullptr || frames == 0) {
        return false;
    }

    const size_t samples = static_cast<size_t>(frames) * m_channels;

    if (!m_inRing.Push(in, samples)) {
        m_overruns.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Same width on both sides: pop straight into the caller's buffer.
    if (m_outChannels == m_channels) {
        if (!m_outRing.Pop(out, samples)) {
            // Worker is late or dead. Fail open: the caller writes the output
            // itself. The quantum just pushed will still come out of the ring
            // later, so a brief doubling is possible -- audible as a glitch,
            // but never silence and never a stall.
            m_underruns.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    // Narrower (or wider) output: stage at input width, then mix down.
    if (frames > m_quantumFrames || !m_outRing.Pop(m_rtScratch, samples)) {
        m_underruns.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    MixChannels(m_rtScratch, m_channels, out, m_outChannels, frames);
    return true;
}

void DspHost::PushRenderRt(const float* in, UINT32 frames, UINT32 channels)
{
    // REAL-TIME PATH. No allocation, no lock, no syscall, no logging.
    if (!m_active.load(std::memory_order_acquire)) {
        return;
    }
    if (in == nullptr || frames == 0 || channels == 0 || frames > m_quantumFrames) {
        return;
    }

    // Downmix here rather than in the DSP: the ring then carries a single
    // channel instead of however many the render endpoint happens to have.
    const float inv = 1.0f / static_cast<float>(channels);
    for (UINT32 frame = 0; frame < frames; ++frame) {
        float sum = 0.0f;
        for (UINT32 ch = 0; ch < channels; ++ch) {
            sum += in[static_cast<size_t>(frame) * channels + ch];
        }
        m_renderMix[frame] = sum * inv;
    }

    // A full ring means the worker is behind on the reference. Dropping the
    // oldest would be better than dropping the newest, but neither is worth a
    // branch here: the canceller re-converges, and it is never fed silence
    // where audio existed -- only less history.
    (void)m_renderRing.Push(m_renderMix, frames);
}

DWORD WINAPI DspHost::WorkerThunk(LPVOID param)
{
    static_cast<DspHost*>(param)->WorkerLoop();
    return 0;
}

void DspHost::WorkerLoop()
{
    // Normal priority by design: the DSP must never compete with audiodg's
    // real-time thread.
    KWIET_LOG("DspHost: worker started");

    // Last value pushed to the engine, so a slider that has not moved costs
    // nothing. -1 forces the first block to publish.
    int32_t appliedAggressiveness = -1;

    while (WaitForSingleObject(m_stopEvent, kWorkerPollMs) == WAIT_TIMEOUT) {
        // Control plane, polled once per wakeup rather than per block.
        if (m_control != nullptr) {
            int32_t tenths = m_control->aggressivenessTenths.load(std::memory_order_relaxed);
            // Clamp: the block is writable by any user process, so treat every
            // value in it as untrusted.
            if (tenths < KWIET_AGGRESSIVENESS_MIN_TENTHS) tenths = KWIET_AGGRESSIVENESS_MIN_TENTHS;
            if (tenths > KWIET_AGGRESSIVENESS_MAX_TENTHS) tenths = KWIET_AGGRESSIVENESS_MAX_TENTHS;
            if (tenths != appliedAggressiveness) {
                m_setAttenuation(m_engine, static_cast<float>(tenths) / 10.0f);
                appliedAggressiveness = tenths;
            }
        }

        // Drain whatever whole blocks are available this wakeup.
        while (m_inRing.Available() >= m_blockSamples) {
            if (m_outRing.Space() < m_blockSamples) {
                // Output ring full: the RT side is not consuming (stream
                // paused). Stop producing rather than spin.
                break;
            }
            if (!m_inRing.Pop(m_scratchIn, m_blockSamples)) {
                break;
            }

            // Two independent off switches: Windows' own effect toggle (SE3)
            // and the Kwiet UI. Either one bypasses.
            const bool uiEnabled = m_control == nullptr
                                   || m_control->enabled.load(std::memory_order_relaxed) != 0;
            const bool bypass = !m_enabled.load(std::memory_order_relaxed) || !uiEnabled
                                || m_dspFailed.load(std::memory_order_relaxed);
            if (bypass) {
                memcpy(m_scratchOut, m_scratchIn, m_blockSamples * sizeof(float));
            } else {
                // The canceller wants one render block per capture block. When
                // the reference is late or absent -- no aux input wired, or the
                // speakers are silent -- feed it silence rather than skip, so
                // its notion of time stays aligned with the capture path.
                if (!m_renderRing.Pop(m_renderScratch, m_blockFrames)) {
                    memset(m_renderScratch, 0, m_blockFrames * sizeof(float));
                }
                m_processRender(m_engine, m_renderScratch, m_blockFrames, 1);

                const int32_t rc = m_process(m_engine, m_scratchIn, m_scratchOut, m_blockFrames);
                if (rc != KWIET_DSP_OK) {
                    // Latch the failure and fall back to passing audio through
                    // untouched for the rest of the session.
                    m_dspErrors.fetch_add(1, std::memory_order_relaxed);
                    m_dspFailed.store(true, std::memory_order_relaxed);
                    KWIET_LOG("DspHost: kwiet_dsp_process failed rc=%ld, bypassing", rc);
                    memcpy(m_scratchOut, m_scratchIn, m_blockSamples * sizeof(float));
                }
            }

            if (!m_outRing.Push(m_scratchOut, m_blockSamples)) {
                break;
            }
        }
    }

    KWIET_LOG("DspHost: worker stopping");
}
