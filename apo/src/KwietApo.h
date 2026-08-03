#pragma once

#include <windows.h>
#include <audioenginebaseapo.h>

#include <atomic>

#include "ControlShm.h"
#include "DspHost.h"
#include "KwietAec.h"
#include "KwietSe3.h"

#include "KwietGuids.h"

// Kwiet passthrough APO (milestone 1).
//
// Copies capture input to output untouched. The real DSP plumbing (SPSC rings
// + worker thread + Rust cdylib) arrives at milestone 2 and must keep
// APOProcess() allocation-free, lock-free, syscall-free and fail-open.
//
// COM aggregation: audiodg creates APOs aggregated (pUnkOuter != nullptr,
// riid = IID_IUnknown), so the class implements the classic inner/outer
// pattern: a non-delegating IUnknown handed to the aggregator, while the
// public interfaces delegate IUnknown calls to the controlling unknown.
class KwietApo final
    : public IAudioProcessingObject
    , public IAudioProcessingObjectRT
    , public IAudioProcessingObjectConfiguration
    , public IAudioSystemEffects3
    , public IAudioProcessingObjectNotifications
    , public IAudioProcessingObjectPreferredFormatSupport
    , public IApoAuxiliaryInputConfiguration
    , public IApoAuxiliaryInputRT
    , public IApoAcousticEchoCancellation
{
public:
    // Factory entry point; supports aggregated and standalone creation.
    static HRESULT Create(IUnknown* pUnkOuter, REFIID riid, void** ppvObject);

    KwietApo(const KwietApo&) = delete;
    KwietApo& operator=(const KwietApo&) = delete;

    // IUnknown (delegating: forwards to the controlling unknown)
    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IAudioProcessingObject
    STDMETHODIMP Reset() override;
    STDMETHODIMP GetLatency(HNSTIME* pTime) override;
    STDMETHODIMP GetRegistrationProperties(APO_REG_PROPERTIES** ppRegProps) override;
    STDMETHODIMP Initialize(UINT32 cbDataSize, BYTE* pbyData) override;
    STDMETHODIMP IsInputFormatSupported(IAudioMediaType* pOppositeFormat,
                                        IAudioMediaType* pRequestedInputFormat,
                                        IAudioMediaType** ppSupportedInputFormat) override;
    STDMETHODIMP IsOutputFormatSupported(IAudioMediaType* pOppositeFormat,
                                         IAudioMediaType* pRequestedOutputFormat,
                                         IAudioMediaType** ppSupportedOutputFormat) override;
    STDMETHODIMP GetInputChannelCount(UINT32* pu32ChannelCount) override;

    // IAudioProcessingObjectConfiguration
    STDMETHODIMP LockForProcess(UINT32 u32NumInputConnections,
                                APO_CONNECTION_DESCRIPTOR** ppInputConnections,
                                UINT32 u32NumOutputConnections,
                                APO_CONNECTION_DESCRIPTOR** ppOutputConnections) override;
    STDMETHODIMP UnlockForProcess() override;

    // IAudioProcessingObjectRT -- real-time path, see rules in APOProcess().
    STDMETHODIMP_(void) APOProcess(UINT32 u32NumInputConnections,
                                   APO_CONNECTION_PROPERTY** ppInputConnections,
                                   UINT32 u32NumOutputConnections,
                                   APO_CONNECTION_PROPERTY** ppOutputConnections) override;
    STDMETHODIMP_(UINT32) CalcInputFrames(UINT32 u32OutputFrameCount) override;
    STDMETHODIMP_(UINT32) CalcOutputFrames(UINT32 u32InputFrameCount) override;

    // IAudioSystemEffects2
    STDMETHODIMP GetEffectsList(LPGUID* ppEffectsIds, UINT* pcEffects, HANDLE Event) override;

    // IAudioSystemEffects3 — Windows 11 requires this for an APO to stay in
    // the graph: the engine drops effect-less/SE2-only APOs at insertion time.
    STDMETHODIMP GetControllableSystemEffectsList(AUDIO_SYSTEMEFFECT** effects,
                                                  UINT* numEffects, HANDLE event) override;
    STDMETHODIMP SetAudioSystemEffectState(GUID effectId,
                                           AUDIO_SYSTEMEFFECT_STATE state) override;

    // IAudioProcessingObjectNotifications — part of the SE3 contract; the
    // passthrough registers for zero notifications.
    STDMETHODIMP GetApoNotificationRegistrationInfo(APO_NOTIFICATION_DESCRIPTOR** apoNotifications,
                                                    DWORD* count) override;
    STDMETHODIMP_(void) HandleNotification(APO_NOTIFICATION* apoNotification) override;

    // IAudioProcessingObjectPreferredFormatSupport — passthrough: the
    // preferred format on one side is whatever the other side uses.
    STDMETHODIMP GetPreferredInputFormat(IAudioMediaType* outputFormat,
                                         IAudioMediaType** preferredFormat) override;
    STDMETHODIMP GetPreferredOutputFormat(IAudioMediaType* inputFormat,
                                          IAudioMediaType** preferredFormat) override;

    // IApoAuxiliaryInputConfiguration — the reference (render) stream used for
    // echo cancellation. Non-RT.
    STDMETHODIMP AddAuxiliaryInput(DWORD dwInputId, UINT32 cbDataSize, BYTE* pbyData,
                                   APO_CONNECTION_DESCRIPTOR* pInputConnection) override;
    STDMETHODIMP RemoveAuxiliaryInput(DWORD dwInputId) override;
    STDMETHODIMP IsInputFormatSupported(IAudioMediaType* pRequestedInputFormat,
                                        IAudioMediaType** ppSupportedInputFormat) override;

    // IApoAuxiliaryInputRT — real-time delivery of the reference audio.
    STDMETHODIMP_(void) AcceptInput(DWORD dwInputId,
                                    const APO_CONNECTION_PROPERTY* pInputConnection) override;

    // IApoAcousticEchoCancellation declares no method: presenting it is the
    // whole point.

private:
    // Non-delegating IUnknown handed to the aggregator; owns the refcount.
    class Inner final : public IUnknown
    {
    public:
        explicit Inner(KwietApo& owner) : m_owner(owner) {}
        STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject) override;
        STDMETHODIMP_(ULONG) AddRef() override;
        STDMETHODIMP_(ULONG) Release() override;

    private:
        KwietApo& m_owner;
    };

    explicit KwietApo(IUnknown* pUnkOuter);
    ~KwietApo();

    Inner m_inner{ *this };
    IUnknown* m_controlling = nullptr;   // pUnkOuter when aggregated, else &m_inner
    std::atomic<ULONG> m_refCount{ 1 };  // owned by Inner

    // m_locked gates the RT path: written by LockForProcess/UnlockForProcess
    // (release), read by APOProcess (acquire) so m_bytesPerFrame is visible.
    std::atomic<bool> m_locked{ false };

    bool   m_initialized = false;
    GUID   m_processingMode{};      // from APOInitSystemEffects2, zero otherwise
    UINT32 m_samplesPerFrame = 0;   // interleaved channel count
    UINT32 m_bytesPerFrame = 0;
    UINT32 m_sampleRate = 0;
    HNSTIME m_latencyHns = 0;       // fixed pipeline delay, 0 until locked

    // User-facing effect state, toggled from Settings through SE3. Forwarded
    // to the DSP worker as a bypass so the pipeline delay stays constant
    // whichever way it is set (GetLatency is only queried once per stream).
    std::atomic<bool> m_effectEnabled{ true };

    // Owns the rings, the worker thread and the Rust cdylib. Started at
    // LockForProcess, stopped at UnlockForProcess; if it fails to start the
    // APO simply stays a passthrough.
    DspHost m_dsp;

    // Reference stream for echo cancellation. Registered by the engine through
    // AddAuxiliaryInput and fed by AcceptInput on the RT thread.
    // MILESTONE: the audio is currently only counted, not yet cancelled.
    static constexpr DWORD kNoAuxInput = 0xFFFFFFFF;
    DWORD  m_auxInputId = kNoAuxInput;
    UINT32 m_auxChannels = 0;
    UINT32 m_auxSampleRate = 0;
    std::atomic<UINT32> m_auxFrames{ 0 };   // frames of reference seen this stream

    // Shared with the UI. Opened alongside the DSP; null when unavailable,
    // in which case the APO runs on its built-in settings.
    ControlShm m_control;
    // Cached so APOProcess never dereferences the ControlShm object itself.
    std::atomic<KwietControlBlock*> m_controlBlock{ nullptr };
};
