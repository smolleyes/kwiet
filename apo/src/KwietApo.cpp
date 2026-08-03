#include "KwietApo.h"

#include <objbase.h>

#include <cstring>

#include "KwietDevLog.h"
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

// Peak of an interleaved block, scaled for the VU meter. RT-safe: a linear
// scan of ~1000 floats per quantum, no branching beyond the compare.
int32_t PeakQ15(const float* samples, size_t count)
{
    float peak = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        const float magnitude = samples[i] < 0.0f ? -samples[i] : samples[i];
        if (magnitude > peak) {
            peak = magnitude;
        }
    }
    if (peak > 1.0f) {
        peak = 1.0f;
    }
    return static_cast<int32_t>(peak * KWIET_PEAK_SCALE);
}

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

KwietApo::KwietApo(IUnknown* pUnkOuter)
    : m_controlling(pUnkOuter != nullptr ? pUnkOuter : &m_inner)
{
    ModuleAddRef();
    KWIET_LOG("KwietApo: instance created (aggregated=%d)", pUnkOuter != nullptr);
}

KwietApo::~KwietApo()
{
    KWIET_LOG("KwietApo: instance destroyed (mode was set=%d)", m_processingMode.Data1 != 0);
    ModuleRelease();
}

HRESULT KwietApo::Create(IUnknown* pUnkOuter, REFIID riid, void** ppvObject)
{
    if (ppvObject == nullptr) {
        return E_POINTER;
    }
    *ppvObject = nullptr;

    // COM rule: an aggregated object may only be asked for IUnknown.
    if (pUnkOuter != nullptr && riid != __uuidof(IUnknown)) {
        return CLASS_E_NOAGGREGATION;
    }

    KwietApo* apo = new (std::nothrow) KwietApo(pUnkOuter);
    if (apo == nullptr) {
        return E_OUTOFMEMORY;
    }

    if (pUnkOuter != nullptr) {
        // Hand out the NON-delegating unknown (refcount 1, owned by caller).
        *ppvObject = static_cast<IUnknown*>(&apo->m_inner);
        return S_OK;
    }

    const HRESULT hr = apo->m_inner.QueryInterface(riid, ppvObject);
    apo->m_inner.Release(); // drop creation ref; destroys the object if QI failed
    return hr;
}

// ---------------------------------------------------------------------------
// IUnknown — non-delegating (aggregation inner)

HRESULT KwietApo::Inner::QueryInterface(REFIID riid, void** ppvObject)
{
    if (ppvObject == nullptr) {
        return E_POINTER;
    }
    *ppvObject = nullptr;

    KwietApo& o = m_owner;
    if (riid == __uuidof(IUnknown)) {
        *ppvObject = static_cast<IUnknown*>(this);
    } else if (riid == __uuidof(IAudioProcessingObject)) {
        KWIET_LOG("QI ok: IAudioProcessingObject");
        *ppvObject = static_cast<IAudioProcessingObject*>(&o);
    } else if (riid == __uuidof(IAudioProcessingObjectRT)) {
        KWIET_LOG("QI ok: IAudioProcessingObjectRT");
        *ppvObject = static_cast<IAudioProcessingObjectRT*>(&o);
    } else if (riid == __uuidof(IAudioProcessingObjectConfiguration)) {
        KWIET_LOG("QI ok: IAudioProcessingObjectConfiguration");
        *ppvObject = static_cast<IAudioProcessingObjectConfiguration*>(&o);
    } else if (riid == __uuidof(IAudioSystemEffects) || riid == __uuidof(IAudioSystemEffects2)
               || riid == __uuidof(IAudioSystemEffects3)) {
        *ppvObject = static_cast<IAudioSystemEffects3*>(&o);
    } else if (riid == __uuidof(IAudioProcessingObjectNotifications)) {
        *ppvObject = static_cast<IAudioProcessingObjectNotifications*>(&o);
    } else if (riid == __uuidof(IAudioProcessingObjectPreferredFormatSupport)) {
        *ppvObject = static_cast<IAudioProcessingObjectPreferredFormatSupport*>(&o);
    } else {
#if defined(KWIET_DEV_LOG)
        char g[40];
        KWIET_LOG("QI: E_NOINTERFACE for %s", KwietGuidToA(riid, g, sizeof(g)));
#endif
        return E_NOINTERFACE;
    }

    // AddRef through the returned pointer: for owner interfaces this goes
    // through the delegating vtable (i.e. the outer when aggregated), per the
    // COM aggregation rules.
    static_cast<IUnknown*>(*ppvObject)->AddRef();
    return S_OK;
}

ULONG KwietApo::Inner::AddRef()
{
    return m_owner.m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG KwietApo::Inner::Release()
{
    const ULONG remaining = m_owner.m_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remaining == 0) {
        delete &m_owner;
    }
    return remaining;
}

// ---------------------------------------------------------------------------
// IUnknown — delegating (public interfaces forward to the controlling unknown)

HRESULT KwietApo::QueryInterface(REFIID riid, void** ppvObject)
{
    return m_controlling->QueryInterface(riid, ppvObject);
}

ULONG KwietApo::AddRef()
{
    return m_controlling->AddRef();
}

ULONG KwietApo::Release()
{
    return m_controlling->Release();
}

// ---------------------------------------------------------------------------
// IAudioProcessingObject

HRESULT KwietApo::Reset()
{
    // The engine calls Reset outside the streaming state; the rings are
    // rebuilt by the next LockForProcess, so there is nothing to flush here.
    return S_OK;
}

HRESULT KwietApo::GetLatency(HNSTIME* pTime)
{
    if (pTime == nullptr) {
        return E_POINTER;
    }
    // Fixed delay of the ring pipeline, so the engine can compensate capture
    // timestamps. Zero until LockForProcess has sized it.
    *pTime = m_latencyHns;
    return S_OK;
}

HRESULT KwietApo::GetRegistrationProperties(APO_REG_PROPERTIES** ppRegProps)
{
    KWIET_LOG("GetRegistrationProperties");
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
    // Identical formats on both sides, and INPLACE: the Windows 11 mode pipe
    // silently drops non-inplace APOs at graph build (every working catalog
    // entry on real hardware carries APO_FLAG_INPLACE).
    props->Flags = static_cast<APO_FLAG>(APO_FLAG_INPLACE | APO_FLAG_DEFAULT);
    wcscpy_s(props->szFriendlyName, L"Kwiet Passthrough APO");
    wcscpy_s(props->szCopyrightInfo, L"Copyright (c) 2026 Kwiet contributors (Apache-2.0)");
    props->u32MajorVersion = 1;
    props->u32MinorVersion = 0;
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
    KWIET_LOG("Initialize: cbDataSize=%u (base=%u, SE=%u, SE2=%u)", cbDataSize,
              static_cast<unsigned>(sizeof(APOInitBaseStruct)),
              static_cast<unsigned>(sizeof(APOInitSystemEffects)),
              static_cast<unsigned>(sizeof(APOInitSystemEffects2)));
    if (pbyData == nullptr || cbDataSize < sizeof(APOInitBaseStruct)) {
        return E_INVALIDARG;
    }

    const auto* base = reinterpret_cast<const APOInitBaseStruct*>(pbyData);
    if (!IsEqualGUID(base->clsid, CLSID_KwietApo)) {
        KWIET_LOG("Initialize: clsid mismatch -> E_INVALIDARG");
        return E_INVALIDARG;
    }

    m_processingMode = GUID{};
    if (cbDataSize == sizeof(APOInitSystemEffects2)) {
        const auto* init2 = reinterpret_cast<const APOInitSystemEffects2*>(pbyData);
        m_processingMode = init2->AudioProcessingMode;
#if defined(KWIET_DEV_LOG)
        char g[40];
        KWIET_LOG("Initialize: SE2, mode=%s, discoveryOnly=%d",
                  KwietGuidToA(init2->AudioProcessingMode, g, sizeof(g)),
                  init2->InitializeForDiscoveryOnly);
#endif
    } else if (cbDataSize == sizeof(APOInitSystemEffects3)) {
        // The engine switches to this layout once the APO exposes SE3.
        const auto* init3 = reinterpret_cast<const APOInitSystemEffects3*>(pbyData);
        m_processingMode = init3->AudioProcessingMode;
#if defined(KWIET_DEV_LOG)
        char g[40];
        KWIET_LOG("Initialize: SE3, mode=%s, discoveryOnly=%d",
                  KwietGuidToA(init3->AudioProcessingMode, g, sizeof(g)),
                  init3->InitializeForDiscoveryOnly);
#endif
    }
    // Property stores / service provider are not retained: the control plane
    // stays shmem-based (milestone 4).

    m_initialized = true;
    KWIET_LOG("Initialize: S_OK");
    return S_OK;
}

HRESULT KwietApo::IsInputFormatSupported(IAudioMediaType* pOppositeFormat,
                                         IAudioMediaType* pRequestedInputFormat,
                                         IAudioMediaType** ppSupportedInputFormat)
{
    KWIET_LOG("IsInputFormatSupported");
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
    m_sampleRate = static_cast<UINT32>(inFmt.fFramesPerSecond);

    // Everything that allocates, loads a DLL or starts a thread happens here,
    // never in APOProcess. A failure is not fatal: the APO degrades to a plain
    // passthrough, which is the whole point of the fail-open design.
    const UINT32 quantumFrames = ppInputConnections[0]->u32MaxFrameCount;
    m_latencyHns = 0;

    KwietControlBlock* control = nullptr;
    if (m_control.Open()) {
        control = m_control.Block();
    }
    m_controlBlock.store(control, std::memory_order_release);

    if (m_dsp.Start(m_sampleRate, m_samplesPerFrame, quantumFrames, control)) {
        m_dsp.SetEnabled(m_effectEnabled.load(std::memory_order_relaxed));
        if (m_sampleRate != 0) {
            m_latencyHns = static_cast<HNSTIME>(m_dsp.LatencyFrames()) * 10000000
                           / static_cast<HNSTIME>(m_sampleRate);
        }
    } else {
        KWIET_LOG("LockForProcess: DSP unavailable, staying in passthrough");
    }

    if (control != nullptr) {
        control->sampleRate.store(static_cast<int32_t>(m_sampleRate), std::memory_order_relaxed);
        control->channels.store(static_cast<int32_t>(m_samplesPerFrame), std::memory_order_relaxed);
        control->latencyFrames.store(static_cast<int32_t>(m_dsp.LatencyFrames()),
                                     std::memory_order_relaxed);
        control->dspActive.store(m_latencyHns != 0 ? 1 : 0, std::memory_order_relaxed);
        control->underruns.store(0, std::memory_order_relaxed);
        control->dspErrors.store(0, std::memory_order_relaxed);
        // Bumping the generation is how the UI learns a new stream started and
        // that it should push its stored settings again.
        control->generation.fetch_add(1, std::memory_order_relaxed);
        control->streaming.store(1, std::memory_order_release);
    }

    m_locked.store(true, std::memory_order_release);
    KWIET_LOG("LockForProcess: S_OK, %u ch, %u Hz, quantum=%u frames, latency=%lld hns, control=%d",
              m_samplesPerFrame, m_sampleRate, quantumFrames,
              static_cast<long long>(m_latencyHns), control != nullptr);
    return S_OK;
}

HRESULT KwietApo::UnlockForProcess()
{
    m_locked.store(false, std::memory_order_release);
    KWIET_LOG("UnlockForProcess: underruns=%u overruns=%u dspErrors=%u",
              m_dsp.Underruns(), m_dsp.Overruns(), m_dsp.DspErrors());

    if (KwietControlBlock* control = m_controlBlock.load(std::memory_order_acquire)) {
        control->underruns.store(static_cast<int32_t>(m_dsp.Underruns()), std::memory_order_relaxed);
        control->dspErrors.store(static_cast<int32_t>(m_dsp.DspErrors()), std::memory_order_relaxed);
        control->peakIn.store(0, std::memory_order_relaxed);
        control->peakOut.store(0, std::memory_order_relaxed);
        control->dspActive.store(0, std::memory_order_relaxed);
        control->streaming.store(0, std::memory_order_release);
    }
    // Clear before closing: APOProcess must never see a stale mapping.
    m_controlBlock.store(nullptr, std::memory_order_release);

    m_dsp.Stop();
    m_control.Close();
    m_latencyHns = 0;
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
        const float* src = reinterpret_cast<const float*>(in->pBuffer);
        float* dst = reinterpret_cast<float*>(out->pBuffer);
        const size_t samples = static_cast<size_t>(frames) * m_samplesPerFrame;

        // VU meters. Measured before the DSP because with APO_FLAG_INPLACE the
        // input buffer may be the output buffer.
        KwietControlBlock* control = m_controlBlock.load(std::memory_order_acquire);
        const int32_t peakIn = (control != nullptr) ? PeakQ15(src, samples) : 0;

        // The DSP pipeline handles the copy when it has audio ready. On any
        // doubt -- not started, ring underrun, worker gone -- it says so and
        // we fall open to a plain copy rather than emit silence or block.
        if (!m_dsp.ProcessRt(src, dst, frames)) {
            if (src != dst) {
                memcpy(dst, src, bytes);
            }
        }

        if (control != nullptr) {
            control->peakIn.store(peakIn, std::memory_order_relaxed);
            control->peakOut.store(PeakQ15(dst, samples), std::memory_order_relaxed);
        }

        out->u32ValidFrameCount = frames;
        out->u32BufferFlags = BUFFER_VALID;
    } else {
        // BUFFER_SILENT: the pipeline is left untouched (nothing pushed, nothing
        // popped) so the rings keep their fill level and audio resumes without
        // an underrun.
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

    KWIET_LOG("GetEffectsList");
    if (ppEffectsIds == nullptr || pcEffects == nullptr) {
        return E_POINTER;
    }
    if (!m_effectEnabled.load(std::memory_order_relaxed)) {
        *ppEffectsIds = nullptr;
        *pcEffects = 0;
        return S_OK;
    }
    auto* ids = static_cast<GUID*>(CoTaskMemAlloc(sizeof(GUID)));
    if (ids == nullptr) {
        return E_OUTOFMEMORY;
    }
    ids[0] = KWIET_EFFECT_NoiseSuppression;
    *ppEffectsIds = ids;
    *pcEffects = 1;
    return S_OK;
}

// ---------------------------------------------------------------------------
// IAudioSystemEffects3

HRESULT KwietApo::GetControllableSystemEffectsList(AUDIO_SYSTEMEFFECT** effects,
                                                   UINT* numEffects, HANDLE event)
{
    UNREFERENCED_PARAMETER(event); // static effect list, no change notification

    KWIET_LOG("GetControllableSystemEffectsList");
    if (effects == nullptr || numEffects == nullptr) {
        return E_POINTER;
    }
    auto* list = static_cast<AUDIO_SYSTEMEFFECT*>(CoTaskMemAlloc(sizeof(AUDIO_SYSTEMEFFECT)));
    if (list == nullptr) {
        return E_OUTOFMEMORY;
    }
    list[0].id = KWIET_EFFECT_NoiseSuppression;
    list[0].canSetState = TRUE;
    list[0].state = m_effectEnabled.load(std::memory_order_relaxed)
                        ? AUDIO_SYSTEMEFFECT_STATE_ON
                        : AUDIO_SYSTEMEFFECT_STATE_OFF;
    *effects = list;
    *numEffects = 1;
    return S_OK;
}

HRESULT KwietApo::SetAudioSystemEffectState(GUID effectId, AUDIO_SYSTEMEFFECT_STATE state)
{
    if (!IsEqualGUID(effectId, KWIET_EFFECT_NoiseSuppression)) {
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    const bool enabled = (state == AUDIO_SYSTEMEFFECT_STATE_ON);
    m_effectEnabled.store(enabled, std::memory_order_relaxed);
    // Bypasses the DSP inside the worker; the pipeline delay is unchanged so
    // the latency reported at stream start stays valid.
    m_dsp.SetEnabled(enabled);
    KWIET_LOG("SetAudioSystemEffectState: enabled=%d", enabled);
    return S_OK;
}

// ---------------------------------------------------------------------------
// IAudioProcessingObjectNotifications

HRESULT KwietApo::GetApoNotificationRegistrationInfo(
    APO_NOTIFICATION_DESCRIPTOR** apoNotifications, DWORD* count)
{
    KWIET_LOG("GetApoNotificationRegistrationInfo");
    if (apoNotifications == nullptr || count == nullptr) {
        return E_POINTER;
    }
    // No notification subscriptions for the passthrough.
    *apoNotifications = nullptr;
    *count = 0;
    return S_OK;
}

void KwietApo::HandleNotification(APO_NOTIFICATION* apoNotification)
{
    UNREFERENCED_PARAMETER(apoNotification); // nothing subscribed
}

// ---------------------------------------------------------------------------
// IAudioProcessingObjectPreferredFormatSupport

HRESULT KwietApo::GetPreferredInputFormat(IAudioMediaType* outputFormat,
                                          IAudioMediaType** preferredFormat)
{
    UNREFERENCED_PARAMETER(outputFormat);
    KWIET_LOG("GetPreferredInputFormat -> E_NOTIMPL (no preference)");
    if (preferredFormat != nullptr) {
        *preferredFormat = nullptr;
    }
    // No format preference: a 1:1 passthrough accepts whatever the pipe uses.
    // Returning S_OK with a format here makes the engine treat the APO as a
    // format converter and drop it from the mode pipe (observed empirically);
    // E_NOTIMPL falls back to the standard negotiation.
    return E_NOTIMPL;
}

HRESULT KwietApo::GetPreferredOutputFormat(IAudioMediaType* inputFormat,
                                           IAudioMediaType** preferredFormat)
{
    UNREFERENCED_PARAMETER(inputFormat);
    KWIET_LOG("GetPreferredOutputFormat -> E_NOTIMPL (no preference)");
    if (preferredFormat != nullptr) {
        *preferredFormat = nullptr;
    }
    return E_NOTIMPL;
}
