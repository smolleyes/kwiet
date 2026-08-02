#pragma once

#include <windows.h>
#include <audioenginebaseapo.h>

#include <atomic>

#include "KwietGuids.h"

// Kwiet passthrough APO (milestone 1).
//
// Copies capture input to output untouched. The real DSP plumbing (SPSC rings
// + worker thread + Rust cdylib) arrives at milestone 2 and must keep
// APOProcess() allocation-free, lock-free, syscall-free and fail-open.
class KwietApo final
    : public IAudioProcessingObject
    , public IAudioProcessingObjectRT
    , public IAudioProcessingObjectConfiguration
    , public IAudioSystemEffects2
{
public:
    KwietApo();

    KwietApo(const KwietApo&) = delete;
    KwietApo& operator=(const KwietApo&) = delete;

    // IUnknown
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

private:
    ~KwietApo();

    std::atomic<ULONG> m_refCount{ 1 };

    // m_locked gates the RT path: written by LockForProcess/UnlockForProcess
    // (release), read by APOProcess (acquire) so m_bytesPerFrame is visible.
    std::atomic<bool> m_locked{ false };

    bool   m_initialized = false;
    GUID   m_processingMode{};      // from APOInitSystemEffects2, zero otherwise
    UINT32 m_samplesPerFrame = 0;   // interleaved channel count
    UINT32 m_bytesPerFrame = 0;
};
