#pragma once

#include <windows.h>
#include <audiomediatype.h>
#include <mmreg.h>

#include <atomic>
#include <new>

// A minimal IAudioMediaType describing one uncompressed float32 format.
//
// The SDK ships CreateAudioMediaTypeFromUncompressedAudioFormat for exactly
// this, in audiomediatypecrt.lib -- which drags in ATL. This project does not
// use ATL anywhere else, and requiring the whole component to build a four-method
// object would be a poor trade. The interface is small enough to implement.
//
// Instances are handed to the audio engine, which owns the reference it is
// given, so lifetime is plain COM refcounting.
class MonoMediaType final : public IAudioMediaType
{
public:
    // Builds a media type from an uncompressed description. Returns nullptr on
    // allocation failure; the caller then reports E_OUTOFMEMORY.
    static MonoMediaType* Create(const UNCOMPRESSEDAUDIOFORMAT& fmt)
    {
        auto* self = new (std::nothrow) MonoMediaType(fmt);
        return self;
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
    {
        if (ppvObject == nullptr) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioMediaType)) {
            *ppvObject = static_cast<IAudioMediaType*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = m_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    // IAudioMediaType
    HRESULT STDMETHODCALLTYPE IsCompressedFormat(BOOL* pfCompressed) override
    {
        if (pfCompressed == nullptr) {
            return E_POINTER;
        }
        *pfCompressed = FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE IsEqual(IAudioMediaType* other, DWORD* pdwFlags) override
    {
        if (pdwFlags == nullptr) {
            return E_POINTER;
        }
        *pdwFlags = 0;
        if (other == nullptr) {
            return E_POINTER;
        }
        UNCOMPRESSEDAUDIOFORMAT theirs{};
        if (FAILED(other->GetUncompressedAudioFormat(&theirs))) {
            return S_FALSE;
        }
        const bool sameType = IsEqualGUID(theirs.guidFormatType, m_format.guidFormatType) != 0;
        const bool sameData = theirs.dwSamplesPerFrame == m_format.dwSamplesPerFrame
                              && theirs.fFramesPerSecond == m_format.fFramesPerSecond
                              && theirs.dwBytesPerSampleContainer
                                     == m_format.dwBytesPerSampleContainer
                              && theirs.dwValidBitsPerSample == m_format.dwValidBitsPerSample
                              && theirs.dwChannelMask == m_format.dwChannelMask;
        if (sameType) {
            *pdwFlags |= AUDIOMEDIATYPE_EQUAL_FORMAT_TYPES;
        }
        if (sameData) {
            *pdwFlags |= AUDIOMEDIATYPE_EQUAL_FORMAT_DATA;
        }
        return (sameType && sameData) ? S_OK : S_FALSE;
    }

    const WAVEFORMATEX* STDMETHODCALLTYPE GetAudioFormat() override
    {
        return &m_wave.Format;
    }

    HRESULT STDMETHODCALLTYPE GetUncompressedAudioFormat(
        UNCOMPRESSEDAUDIOFORMAT* pUncompressedAudioFormat) override
    {
        if (pUncompressedAudioFormat == nullptr) {
            return E_POINTER;
        }
        *pUncompressedAudioFormat = m_format;
        return S_OK;
    }

private:
    explicit MonoMediaType(const UNCOMPRESSEDAUDIOFORMAT& fmt)
        : m_format(fmt)
    {
        const WORD channels = static_cast<WORD>(fmt.dwSamplesPerFrame);
        const WORD containerBits = static_cast<WORD>(fmt.dwBytesPerSampleContainer * 8);
        const DWORD rate = static_cast<DWORD>(fmt.fFramesPerSecond);
        const WORD blockAlign = static_cast<WORD>(channels * fmt.dwBytesPerSampleContainer);

        m_wave.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        m_wave.Format.nChannels = channels;
        m_wave.Format.nSamplesPerSec = rate;
        m_wave.Format.nAvgBytesPerSec = rate * blockAlign;
        m_wave.Format.nBlockAlign = blockAlign;
        m_wave.Format.wBitsPerSample = containerBits;
        m_wave.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        m_wave.Samples.wValidBitsPerSample = static_cast<WORD>(fmt.dwValidBitsPerSample);
        m_wave.dwChannelMask = fmt.dwChannelMask;
        m_wave.SubFormat = fmt.guidFormatType;
    }

    ~MonoMediaType() = default;

    MonoMediaType(const MonoMediaType&) = delete;
    MonoMediaType& operator=(const MonoMediaType&) = delete;

    std::atomic<ULONG> m_refCount{ 1 };
    UNCOMPRESSEDAUDIOFORMAT m_format{};
    WAVEFORMATEXTENSIBLE m_wave{};
};
