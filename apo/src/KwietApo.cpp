#include "KwietApo.h"

#include <objbase.h>

#include <cstring>

#include "Module.h"

namespace {

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, defined locally to avoid ksmedia.h.
constexpr GUID kIeeeFloatSubtype = {
    0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

#ifdef APOERR_FORMAT_NOT_SUPPORTED
constexpr HRESULT kFormatNotSupported = APOERR_FORMAT_NOT_SUPPORTED;
#else
constexpr HRESULT kFormatNotSupported = E_FAIL;
#endif

// The shared-mode engine feeds APOs interleaved float32; anything else is refused.
bool IsFloat32(IAudioMediaType* type, UNCOMPRESSEDAUDIOFORMAT* outFmt = nullptr)
{
    if (type == nullptr) {
        return false;
    }
    UNCOMPRESSEDAUDIOFORMAT fmt{};
    if (FAILED(type->GetUncompressedAudioFormat(&fmt))) {
        return false;
    }
    if (outFmt != nullptr) {
        *outFmt = fmt;
    }
    return IsEqualGUID(fmt.guidFormatType, kIeeeFloatSubtype)
        && fmt.dwBytesPerSampleContainer == sizeof(float);
}

} // namespace

KwietApo::KwietApo()
{
    ModuleAddRef();
}

KwietApo::~KwietApo()
{
    ModuleRelease();
}

// ---------------------------------------------------------------------------
// IUnknown

HRESULT KwietApo::QueryInterface(REFIID riid, void** ppvObject)
{
    if (ppvObject == nullptr) {
        return E_POINTER;
    }
    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioProcessingObject)) {
        *ppvObject = static_cast<IAudioProcessingObject*>(this);
    } else if (riid == __uuidof(IAudioProcessingObjectRT)) {
        *ppvObject = static_cast<IAudioProcessingObjectRT*>(this);
    } else if (riid == __uuidof(IAudioProcessingObjectConfiguration)) {
        *ppvObject = static_cast<IAudioProcessingObjectConfiguration*>(this);
    } else if (riid == __uuidof(IAudioSystemEffects) || riid == __uuidof(IAudioSystemEffects2)) {
        *ppvObject = static_cast<IAudioSystemEffects2*>(this);
    } else {
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

ULONG KwietApo::AddRef()
{
    return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG KwietApo::Release()
{
    const ULONG remaining = m_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remaining == 0) {
        delete this;
    }
    return remaining;
}

// ---------------------------------------------------------------------------
// IAudioProcessingObject

HRESULT KwietApo::Reset()
{
    // Passthrough holds no history. Milestone 2: flush rings + DSP state here.
    return S_OK;
}

HRESULT KwietApo::GetLatency(HNSTIME* pTime)
{
    if (pTime == nullptr) {
        return E_POINTER;
    }
    // Pure passthrough: no algorithmic delay. Milestone 2 reports the fixed
    // ring/lookahead delay here so the engine can compensate timestamps.
    *pTime = 0;
    return S_OK;
}

HRESULT KwietApo::GetRegistrationProperties(APO_REG_PROPERTIES** ppRegProps)
{
    if (ppRegProps == nullptr) {
        return E_POINTER;
    }
    *ppRegProps = nullptr;

    constexpr UINT32 kNumInterfaces = 3;
    const size_t cb = sizeof(APO_REG_PROPERTIES) + (kNumInterfaces - 1) * sizeof(IID);
    auto* props = static_cast<APO_REG_PROPERTIES*>(CoTaskMemAlloc(cb));
    if (props == nullptr) {
        return E_OUTOFMEMORY;
    }
    ZeroMemory(props, cb);

    props->clsid = CLSID_KwietApo;
    // Engine must give us identical formats on both sides; no format conversion
    // in the APO itself.
    props->Flags = APO_FLAG_DEFAULT;
    wcscpy_s(props->szFriendlyName, L"Kwiet Passthrough APO");
    wcscpy_s(props->szCopyrightInfo, L"Copyright (c) 2026 Kwiet contributors (Apache-2.0)");
    props->u32MajorVersion = 0;
    props->u32MinorVersion = 1;
    props->u32MinInputConnections = 1;
    props->u32MaxInputConnections = 1;
    props->u32MinOutputConnections = 1;
    props->u32MaxOutputConnections = 1;
    props->u32MaxInstances = 0xFFFFFFFF;
    props->u32NumAPOInterfaces = kNumInterfaces;
    props->iidAPOInterfaceList[0] = __uuidof(IAudioProcessingObject);
    props->iidAPOInterfaceList[1] = __uuidof(IAudioProcessingObjectRT);
    props->iidAPOInterfaceList[2] = __uuidof(IAudioProcessingObjectConfiguration);

    *ppRegProps = props;
    return S_OK;
}

HRESULT KwietApo::Initialize(UINT32 cbDataSize, BYTE* pbyData)
{
    if (pbyData == nullptr || cbDataSize < sizeof(APOInitBaseStruct)) {
        return E_INVALIDARG;
    }

    const auto* base = reinterpret_cast<const APOInitBaseStruct*>(pbyData);
    if (!IsEqualGUID(base->clsid, CLSID_KwietApo)) {
        return E_INVALIDARG;
    }

    m_processingMode = GUID{};
    if (cbDataSize == sizeof(APOInitSystemEffects2)) {
        const auto* init2 = reinterpret_cast<const APOInitSystemEffects2*>(pbyData);
        m_processingMode = init2->AudioProcessingMode;
        // init2->InitializeForDiscoveryOnly: instantiated for enumeration only;
        // nothing extra to do for a passthrough.
    }
    // TODO(milestone 2): handle APOInitSystemEffects3 (Win11 22H2+, different
    // layout, do NOT cast by size >=) and read endpoint property stores if the
    // DSP ever needs per-endpoint settings. Control plane stays shmem-based.

    m_initialized = true;
    return S_OK;
}

HRESULT KwietApo::IsInputFormatSupported(IAudioMediaType* pOppositeFormat,
                                         IAudioMediaType* pRequestedInputFormat,
                                         IAudioMediaType** ppSupportedInputFormat)
{
    if (pRequestedInputFormat == nullptr || ppSupportedInputFormat == nullptr) {
        return E_POINTER;
    }
    *ppSupportedInputFormat = nullptr;

    if (IsFloat32(pRequestedInputFormat)) {
        *ppSupportedInputFormat = pRequestedInputFormat;
        pRequestedInputFormat->AddRef();
        return S_OK;
    }
    // Not supported as requested: suggest the opposite side's format if usable.
    if (IsFloat32(pOppositeFormat)) {
        *ppSupportedInputFormat = pOppositeFormat;
        pOppositeFormat->AddRef();
        return S_FALSE;
    }
    return kFormatNotSupported;
}

HRESULT KwietApo::IsOutputFormatSupported(IAudioMediaType* pOppositeFormat,
                                          IAudioMediaType* pRequestedOutputFormat,
                                          IAudioMediaType** ppSupportedOutputFormat)
{
    if (pRequestedOutputFormat == nullptr || ppSupportedOutputFormat == nullptr) {
        return E_POINTER;
    }
    *ppSupportedOutputFormat = nullptr;

    if (IsFloat32(pRequestedOutputFormat)) {
        *ppSupportedOutputFormat = pRequestedOutputFormat;
        pRequestedOutputFormat->AddRef();
        return S_OK;
    }
    if (IsFloat32(pOppositeFormat)) {
        *ppSupportedOutputFormat = pOppositeFormat;
        pOppositeFormat->AddRef();
        return S_FALSE;
    }
    return kFormatNotSupported;
}

HRESULT KwietApo::GetInputChannelCount(UINT32* pu32ChannelCount)
{
    if (pu32ChannelCount == nullptr) {
        return E_POINTER;
    }
    if (!m_locked.load(std::memory_order_acquire)) {
        // Only meaningful once the format is locked.
        return E_FAIL;
    }
    *pu32ChannelCount = m_samplesPerFrame;
    return S_OK;
}

// ---------------------------------------------------------------------------
// IAudioProcessingObjectConfiguration

HRESULT KwietApo::LockForProcess(UINT32 u32NumInputConnections,
                                 APO_CONNECTION_DESCRIPTOR** ppInputConnections,
                                 UINT32 u32NumOutputConnections,
                                 APO_CONNECTION_DESCRIPTOR** ppOutputConnections)
{
    if (ppInputConnections == nullptr || ppOutputConnections == nullptr) {
        return E_POINTER;
    }
    if (u32NumInputConnections != 1 || u32NumOutputConnections != 1) {
        return E_INVALIDARG;
    }
    if (ppInputConnections[0] == nullptr || ppOutputConnections[0] == nullptr
        || ppInputConnections[0]->pFormat == nullptr
        || ppOutputConnections[0]->pFormat == nullptr) {
        return E_POINTER;
    }

    UNCOMPRESSEDAUDIOFORMAT inFmt{};
    UNCOMPRESSEDAUDIOFORMAT outFmt{};
    if (!IsFloat32(ppInputConnections[0]->pFormat, &inFmt)
        || !IsFloat32(ppOutputConnections[0]->pFormat, &outFmt)) {
        return kFormatNotSupported;
    }
    // APO_FLAG_DEFAULT should guarantee this, but a passthrough that guessed
    // wrong would corrupt audio, so verify.
    if (inFmt.dwSamplesPerFrame != outFmt.dwSamplesPerFrame
        || inFmt.dwBytesPerSampleContainer != outFmt.dwBytesPerSampleContainer
        || inFmt.fFramesPerSecond != outFmt.fFramesPerSecond) {
        return kFormatNotSupported;
    }

    m_samplesPerFrame = inFmt.dwSamplesPerFrame;
    m_bytesPerFrame = inFmt.dwSamplesPerFrame * inFmt.dwBytesPerSampleContainer;

    // Milestone 2: allocate SPSC rings and start the worker thread HERE
    // (non-RT context); APOProcess must never allocate.

    m_locked.store(true, std::memory_order_release);
    return S_OK;
}

HRESULT KwietApo::UnlockForProcess()
{
    m_locked.store(false, std::memory_order_release);
    // Milestone 2: stop the worker and free rings here.
    return S_OK;
}

// ---------------------------------------------------------------------------
// IAudioProcessingObjectRT

void KwietApo::APOProcess(UINT32 u32NumInputConnections,
                          APO_CONNECTION_PROPERTY** ppInputConnections,
                          UINT32 u32NumOutputConnections,
                          APO_CONNECTION_PROPERTY** ppOutputConnections)
{
    // REAL-TIME PATH (audiodg RT thread, ~10 ms period).
    // Absolute rules: no allocation, no locks, no syscalls, no logging, no COM.
    // Failure policy is fail-open: when in doubt, copy input to output and
    // return; never block, never emit garbage.

    if (u32NumInputConnections == 0 || u32NumOutputConnections == 0
        || ppInputConnections == nullptr || ppOutputConnections == nullptr) {
        return;
    }
    if (!m_locked.load(std::memory_order_acquire)) {
        return;
    }

    const APO_CONNECTION_PROPERTY* in = ppInputConnections[0];
    APO_CONNECTION_PROPERTY* out = ppOutputConnections[0];
    if (in == nullptr || out == nullptr) {
        return;
    }

    const UINT32 frames = in->u32ValidFrameCount;
    const size_t bytes = static_cast<size_t>(frames) * m_bytesPerFrame;

    if (in->u32BufferFlags == BUFFER_VALID) {
        const void* src = reinterpret_cast<const void*>(in->pBuffer);
        void* dst = reinterpret_cast<void*>(out->pBuffer);
        if (src != dst) {
            memcpy(dst, src, bytes);
        }
        out->u32ValidFrameCount = frames;
        out->u32BufferFlags = BUFFER_VALID;
    } else {
        // BUFFER_SILENT (or anything unexpected): write real silence AND set
        // the flag; downstream is not required to honor the flag alone.
        memset(reinterpret_cast<void*>(out->pBuffer), 0, bytes);
        out->u32ValidFrameCount = frames;
        out->u32BufferFlags = BUFFER_SILENT;
    }
}

UINT32 KwietApo::CalcInputFrames(UINT32 u32OutputFrameCount)
{
    // 1:1, no rate conversion.
    return u32OutputFrameCount;
}

UINT32 KwietApo::CalcOutputFrames(UINT32 u32InputFrameCount)
{
    return u32InputFrameCount;
}

// ---------------------------------------------------------------------------
// IAudioSystemEffects2

HRESULT KwietApo::GetEffectsList(LPGUID* ppEffectsIds, UINT* pcEffects, HANDLE Event)
{
    UNREFERENCED_PARAMETER(Event); // static effect list, no change notification

    if (ppEffectsIds == nullptr || pcEffects == nullptr) {
        return E_POINTER;
    }
    // The passthrough advertises no effect. Milestone 2 returns
    // KWIET_EFFECT_NoiseSuppression here when the DSP is enabled.
    *ppEffectsIds = nullptr;
    *pcEffects = 0;
    return S_OK;
}
