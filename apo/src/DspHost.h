#pragma once

#include <windows.h>

#include <atomic>

#include "KwietControl.h"
#include "SpscRing.h"
#include "kwiet_dsp.h"

// Hosts the Rust DSP cdylib off the real-time thread.
//
// APOProcess only ever touches ProcessRt(), which pushes the captured quantum
// into an SPSC ring and pops an already-processed quantum out of a second one.
// A worker thread at normal priority sits between them and is the only thing
// that calls into the DSP.
//
// Failure policy is fail-open at every level: if the library is missing, the
// engine refuses a format, the worker dies, or a ring under-runs, ProcessRt()
// returns false and the APO copies input to output.
class DspHost final
{
public:
    DspHost() = default;

    DspHost(const DspHost&) = delete;
    DspHost& operator=(const DspHost&) = delete;

    ~DspHost()
    {
        Stop();
    }

    // Non-RT (LockForProcess): loads the DLL, creates the engine, allocates
    // the rings and starts the worker. Returns false if the DSP path could not
    // be brought up; the APO then stays in permanent passthrough, which is a
    // valid degraded mode, not an error.
    // `control` may be null: the DSP then runs on its built-in settings and
    // the UI simply has nothing to steer.
    bool Start(UINT32 sampleRate, UINT32 channels, UINT32 framesPerQuantum,
               KwietControlBlock* control);

    // Non-RT (UnlockForProcess): stops the worker and releases everything.
    // Safe to call when not started.
    void Stop();

    // Fixed algorithmic delay introduced by the pipeline, in frames.
    UINT32 LatencyFrames() const
    {
        return m_latencyFrames;
    }

    // RT-SAFE. No allocation, no lock, no syscall.
    // Returns true when `out` holds processed audio; false means the caller
    // must fail open and copy input to output itself.
    bool ProcessRt(const float* in, float* out, UINT32 frames);

    // Control plane; safe from any thread at any time.
    void SetEnabled(bool enabled)
    {
        m_enabled.store(enabled, std::memory_order_relaxed);
    }

    void SetAttenuationDb(float db);

    // Diagnostics, read at Stop() for the dev log.
    UINT32 Underruns() const { return m_underruns.load(std::memory_order_relaxed); }
    UINT32 Overruns() const { return m_overruns.load(std::memory_order_relaxed); }
    UINT32 DspErrors() const { return m_dspErrors.load(std::memory_order_relaxed); }

private:
    using AbiVersionFn = decltype(&kwiet_dsp_abi_version);
    using CreateFn = decltype(&kwiet_dsp_create);
    using DestroyFn = decltype(&kwiet_dsp_destroy);
    using BlockFramesFn = decltype(&kwiet_dsp_block_frames);
    using ProcessFn = decltype(&kwiet_dsp_process);
    using SetAttenuationFn = decltype(&kwiet_dsp_set_attenuation_db);

    static DWORD WINAPI WorkerThunk(LPVOID param);
    void WorkerLoop();
    bool LoadLibraryFromModuleDir();

    HMODULE m_dspModule = nullptr;
    AbiVersionFn m_abiVersion = nullptr;
    CreateFn m_create = nullptr;
    DestroyFn m_destroy = nullptr;
    BlockFramesFn m_blockFramesFn = nullptr;
    ProcessFn m_process = nullptr;
    SetAttenuationFn m_setAttenuation = nullptr;

    KwietDsp* m_engine = nullptr;

    SpscRing m_inRing;
    SpscRing m_outRing;

    float* m_scratchIn = nullptr;
    float* m_scratchOut = nullptr;

    HANDLE m_worker = nullptr;
    HANDLE m_stopEvent = nullptr;

    UINT32 m_channels = 0;
    UINT32 m_quantumFrames = 0;    // what the RT thread pushes/pops each pass
    UINT32 m_blockFrames = 0;      // what the DSP consumes per inference
    size_t m_blockSamples = 0;
    UINT32 m_latencyFrames = 0;

    // Gates the RT path. Release-stored once everything is ready, cleared
    // first thing in Stop().
    std::atomic<bool> m_active{ false };
    std::atomic<bool> m_enabled{ true };
    // Read by the worker every block; null when the control plane is down.
    KwietControlBlock* m_control = nullptr;
    // Latched when the DSP misbehaves: the worker then passes audio through
    // untouched instead of calling into it again.
    std::atomic<bool> m_dspFailed{ false };

    std::atomic<UINT32> m_underruns{ 0 };
    std::atomic<UINT32> m_overruns{ 0 };
    std::atomic<UINT32> m_dspErrors{ 0 };
};
