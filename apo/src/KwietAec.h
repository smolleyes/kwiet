#pragma once

// Minimal, verbatim extracts of the AEC-related APO interfaces from a recent
// audioenginebaseapo.h (microsoft/win32metadata, MIT). The local build SDK
// (10.0.19041) predates them, and the full header does not compile against it
// — same reasoning as KwietSe3.h. Delete once the toolchain uses an SDK
// >= 10.0.22000.
//
// WHY THESE MATTER: the Windows audio engine refuses to put an APO into the
// COMMUNICATIONS pipe unless it presents itself as an echo canceller. Every
// conferencing app (Meet, Teams, Zoom, Discord) opens capture in that
// category, so without these interfaces the APO is instantiated and then
// dropped before LockForProcess. See docs/architecture.md §11.

#include <audioenginebaseapo.h>

#ifndef __IApoAuxiliaryInputConfiguration_INTERFACE_DEFINED__
#define __IApoAuxiliaryInputConfiguration_INTERFACE_DEFINED__

EXTERN_C const IID IID_IApoAuxiliaryInputConfiguration;

// Non-RT configuration of the reference (render) stream.
MIDL_INTERFACE("4CEB0AAB-FA19-48ED-A857-87771AE1B768")
IApoAuxiliaryInputConfiguration : public IUnknown
{
public:
    virtual HRESULT STDMETHODCALLTYPE AddAuxiliaryInput(
        _In_ DWORD dwInputId,
        _In_ UINT32 cbDataSize,
        _In_reads_(cbDataSize) BYTE * pbyData,
        _In_ APO_CONNECTION_DESCRIPTOR * pInputConnection) = 0;

    virtual HRESULT STDMETHODCALLTYPE RemoveAuxiliaryInput(_In_ DWORD dwInputId) = 0;

    virtual HRESULT STDMETHODCALLTYPE IsInputFormatSupported(
        _In_ IAudioMediaType * pRequestedInputFormat,
        _Out_ IAudioMediaType * *ppSupportedInputFormat) = 0;
};

#endif // __IApoAuxiliaryInputConfiguration_INTERFACE_DEFINED__

#ifndef __IApoAuxiliaryInputRT_INTERFACE_DEFINED__
#define __IApoAuxiliaryInputRT_INTERFACE_DEFINED__

EXTERN_C const IID IID_IApoAuxiliaryInputRT;

// Delivers the reference audio. Called on the real-time thread, separately
// from APOProcess: same rules apply (no allocation, no lock, no syscall).
MIDL_INTERFACE("F851809C-C177-49A0-B1B2-B66F017943AB")
IApoAuxiliaryInputRT : public IUnknown
{
public:
    virtual void STDMETHODCALLTYPE AcceptInput(
        _In_ DWORD dwInputId,
        _In_ const APO_CONNECTION_PROPERTY * pInputConnection) = 0;
};

#endif // __IApoAuxiliaryInputRT_INTERFACE_DEFINED__

#ifndef __IApoAcousticEchoCancellation_INTERFACE_DEFINED__
#define __IApoAcousticEchoCancellation_INTERFACE_DEFINED__

EXTERN_C const IID IID_IApoAcousticEchoCancellation;

// Marker interface: it declares no method at all. Presenting it is what tells
// the engine "this APO is the echo canceller for the communications pipe".
MIDL_INTERFACE("25385759-3236-4101-A943-25693DFB5D2D")
IApoAcousticEchoCancellation : public IUnknown
{
};

#endif // __IApoAcousticEchoCancellation_INTERFACE_DEFINED__
