#pragma once

// Minimal, verbatim extract of IAudioSystemEffects3 and its types from
// audioengineextensionapo.h (microsoft/win32metadata, MIT — full reference
// copy in third_party/winsdk/). The local build SDK (10.0.19041) predates
// this interface, and the full header does not compile against it. Delete
// this file and include <audioengineextensionapo.h> once the toolchain uses
// an SDK >= 10.0.22000.

#include <audioenginebaseapo.h>
#include <mmdeviceapi.h>   // IMMDeviceCollection (APOInitSystemEffects3)
#include <servprov.h>      // IServiceProvider (APOInitSystemEffects3)

#ifndef __IAudioSystemEffects3_INTERFACE_DEFINED__
#define __IAudioSystemEffects3_INTERFACE_DEFINED__

typedef /* [v1_enum] */
enum AUDIO_SYSTEMEFFECT_STATE
    {
        AUDIO_SYSTEMEFFECT_STATE_OFF = 0,
        AUDIO_SYSTEMEFFECT_STATE_ON = ( AUDIO_SYSTEMEFFECT_STATE_OFF + 1 )
    } AUDIO_SYSTEMEFFECT_STATE;

typedef struct AUDIO_SYSTEMEFFECT
    {
    GUID id;
    BOOL canSetState;
    AUDIO_SYSTEMEFFECT_STATE state;
    } AUDIO_SYSTEMEFFECT;

EXTERN_C const IID IID_IAudioSystemEffects3;

MIDL_INTERFACE("C58B31CD-FC6A-4255-BC1F-AD29BB0A4A17")
IAudioSystemEffects3 : public IAudioSystemEffects2
{
public:
    virtual HRESULT STDMETHODCALLTYPE GetControllableSystemEffectsList(
        _Outptr_result_buffer_maybenull_(*numEffects)  AUDIO_SYSTEMEFFECT **effects,
        _Out_  UINT *numEffects,
        _In_opt_  HANDLE event) = 0;

    virtual HRESULT STDMETHODCALLTYPE SetAudioSystemEffectState(
        _In_  GUID effectId,
        _In_  AUDIO_SYSTEMEFFECT_STATE state) = 0;
};

#endif // __IAudioSystemEffects3_INTERFACE_DEFINED__

#ifndef KWIET_APOINIT_SE3_DEFINED
#define KWIET_APOINIT_SE3_DEFINED

// Passed to IAudioProcessingObject::Initialize by the Windows 11 engine when
// the APO implements IAudioSystemEffects3 (verbatim from the official header).
typedef struct APOInitSystemEffects3
    {
    APOInitBaseStruct APOInit;
    IPropertyStore *pAPOEndpointProperties;
    IServiceProvider *pServiceProvider;
    IMMDeviceCollection *pDeviceCollection;
    UINT nSoftwareIoDeviceInCollection;
    UINT nSoftwareIoConnectorIndex;
    GUID AudioProcessingMode;
    BOOL InitializeForDiscoveryOnly;
    } APOInitSystemEffects3;

#endif // KWIET_APOINIT_SE3_DEFINED

#ifndef __IAudioProcessingObjectNotifications_INTERFACE_DEFINED__
#define __IAudioProcessingObjectNotifications_INTERFACE_DEFINED__

// Only referenced through pointers here (we register for zero notifications),
// so forward declarations are enough; the full unions live in the reference
// header under third_party/winsdk/.
typedef struct APO_NOTIFICATION_DESCRIPTOR APO_NOTIFICATION_DESCRIPTOR;
typedef struct APO_NOTIFICATION APO_NOTIFICATION;

EXTERN_C const IID IID_IAudioProcessingObjectNotifications;

MIDL_INTERFACE("56B0C76F-02FD-4B21-A52E-9F8219FC86E4")
IAudioProcessingObjectNotifications : public IUnknown
{
public:
    virtual HRESULT STDMETHODCALLTYPE GetApoNotificationRegistrationInfo(
        _Out_writes_(*count)  APO_NOTIFICATION_DESCRIPTOR **apoNotifications,
        _Out_  DWORD *count) = 0;

    virtual void STDMETHODCALLTYPE HandleNotification(
        _In_  APO_NOTIFICATION *apoNotification) = 0;
};

#endif // __IAudioProcessingObjectNotifications_INTERFACE_DEFINED__

#ifndef __IAudioProcessingObjectPreferredFormatSupport_INTERFACE_DEFINED__
#define __IAudioProcessingObjectPreferredFormatSupport_INTERFACE_DEFINED__

EXTERN_C const IID IID_IAudioProcessingObjectPreferredFormatSupport;

MIDL_INTERFACE("51CBD3C4-F1F3-4D2F-A0E1-7E9C4DD0FEB3")
IAudioProcessingObjectPreferredFormatSupport : public IUnknown
{
public:
    virtual HRESULT STDMETHODCALLTYPE GetPreferredInputFormat(
        _In_  IAudioMediaType *outputFormat,
        _Out_  IAudioMediaType **preferredFormat) = 0;

    virtual HRESULT STDMETHODCALLTYPE GetPreferredOutputFormat(
        _In_  IAudioMediaType *inputFormat,
        _Out_  IAudioMediaType **preferredFormat) = 0;
};

#endif // __IAudioProcessingObjectPreferredFormatSupport_INTERFACE_DEFINED__
