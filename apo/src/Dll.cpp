// COM plumbing for the Kwiet APO DLL: class factory, self-registration under
// HKLM\Software\Classes (audiodg reads the merged HKCR view), module lifetime.
//
// Endpoint FX registration (MMDevices\...\FxProperties) is deliberately NOT
// done here: it is policy, owned by installer/install.ps1.

#include <windows.h>

#include <objbase.h>

#include <atomic>
#include <cwchar>
#include <new>

#include "KwietApo.h"
#include "KwietDevLog.h"
#include "KwietGuids.h"
#include "Module.h"

HMODULE g_kwietModule = nullptr;

namespace {
std::atomic<LONG> g_moduleRefs{ 0 };
}

void ModuleAddRef()
{
    g_moduleRefs.fetch_add(1, std::memory_order_relaxed);
}

void ModuleRelease()
{
    g_moduleRefs.fetch_sub(1, std::memory_order_release);
}

// ---------------------------------------------------------------------------

namespace {

class KwietClassFactory final : public IClassFactory
{
public:
    KwietClassFactory()
    {
        ModuleAddRef();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject) override
    {
        if (ppvObject == nullptr) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory)) {
            *ppvObject = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
#if defined(KWIET_DEV_LOG)
        char g[40];
        KWIET_LOG("Factory QI: E_NOINTERFACE for %s", KwietGuidToA(riid, g, sizeof(g)));
#endif
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG remaining = m_refCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject) override
    {
#if defined(KWIET_DEV_LOG)
        char g[40];
        KWIET_LOG("Factory CreateInstance: riid=%s outer=%p", KwietGuidToA(riid, g, sizeof(g)), pUnkOuter);
#endif
        // audiodg aggregates APOs (outer != nullptr, riid = IID_IUnknown).
        const HRESULT hr = KwietApo::Create(pUnkOuter, riid, ppvObject);
        KWIET_LOG("Factory CreateInstance: hr=0x%08lX", static_cast<unsigned long>(hr));
        return hr;
    }

    STDMETHODIMP LockServer(BOOL fLock) override
    {
        if (fLock) {
            ModuleAddRef();
        } else {
            ModuleRelease();
        }
        return S_OK;
    }

private:
    ~KwietClassFactory()
    {
        ModuleRelease();
    }

    std::atomic<ULONG> m_refCount{ 1 };
};

HRESULT WriteRegSz(HKEY key, const wchar_t* valueName, const wchar_t* data)
{
    const auto cb = static_cast<DWORD>((wcslen(data) + 1) * sizeof(wchar_t));
    const LSTATUS st = RegSetValueExW(key, valueName, 0, REG_SZ,
                                      reinterpret_cast<const BYTE*>(data), cb);
    return HRESULT_FROM_WIN32(st);
}

HRESULT WriteRegDword(HKEY key, const wchar_t* valueName, DWORD data)
{
    const LSTATUS st = RegSetValueExW(key, valueName, 0, REG_DWORD,
                                      reinterpret_cast<const BYTE*>(&data), sizeof(data));
    return HRESULT_FROM_WIN32(st);
}

HRESULT BuildClsidKeyPath(wchar_t* buffer, size_t bufferCount)
{
    wchar_t clsidStr[64] = {};
    if (StringFromGUID2(CLSID_KwietApo, clsidStr, ARRAYSIZE(clsidStr)) == 0) {
        return E_UNEXPECTED;
    }
    if (swprintf_s(buffer, bufferCount, L"SOFTWARE\\Classes\\CLSID\\%s", clsidStr) < 0) {
        return E_UNEXPECTED;
    }
    return S_OK;
}

// AudioEndpointBuilder resolves endpoint FX against this catalog (equivalent of
// the samples' RegisterAPO()). Keep the values in sync with
// KwietApo::GetRegistrationProperties.
HRESULT BuildApoCatalogKeyPath(wchar_t* buffer, size_t bufferCount)
{
    wchar_t clsidStr[64] = {};
    if (StringFromGUID2(CLSID_KwietApo, clsidStr, ARRAYSIZE(clsidStr)) == 0) {
        return E_UNEXPECTED;
    }
    if (swprintf_s(buffer, bufferCount,
                   L"SOFTWARE\\Classes\\AudioEngine\\AudioProcessingObjects\\%s", clsidStr) < 0) {
        return E_UNEXPECTED;
    }
    return S_OK;
}

HRESULT RegisterApoCatalog()
{
    wchar_t keyPath[160] = {};
    HRESULT hr = BuildApoCatalogKeyPath(keyPath, ARRAYSIZE(keyPath));
    if (FAILED(hr)) {
        return hr;
    }

    HKEY key = nullptr;
    const LSTATUS st = RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyPath, 0, nullptr, 0,
                                       KEY_WRITE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
    if (st != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(st);
    }

    hr = WriteRegSz(key, L"FriendlyName", L"Kwiet Passthrough APO");
    if (SUCCEEDED(hr)) hr = WriteRegSz(key, L"Copyright", L"Copyright (c) 2026 Kwiet contributors (Apache-2.0)");
    if (SUCCEEDED(hr)) hr = WriteRegDword(key, L"MajorVersion", 1);
    if (SUCCEEDED(hr)) hr = WriteRegDword(key, L"MinorVersion", 0);
    if (SUCCEEDED(hr)) hr = WriteRegDword(key, L"Flags", APO_FLAG_INPLACE | APO_FLAG_DEFAULT);
    if (SUCCEEDED(hr)) hr = WriteRegDword(key, L"MinInputConnections", 1);
    if (SUCCEEDED(hr)) hr = WriteRegDword(key, L"MaxInputConnections", 1);
    if (SUCCEEDED(hr)) hr = WriteRegDword(key, L"MinOutputConnections", 1);
    if (SUCCEEDED(hr)) hr = WriteRegDword(key, L"MaxOutputConnections", 1);
    if (SUCCEEDED(hr)) hr = WriteRegDword(key, L"MaxInstances", 0xFFFFFFFF);
    if (SUCCEEDED(hr)) hr = WriteRegDword(key, L"NumAPOInterfaces", 3);

    const IID interfaces[3] = {
        __uuidof(IAudioProcessingObject),
        __uuidof(IAudioProcessingObjectRT),
        __uuidof(IAudioProcessingObjectConfiguration),
    };
    for (int i = 0; SUCCEEDED(hr) && i < 3; ++i) {
        wchar_t iidStr[64] = {};
        wchar_t valueName[24] = {};
        if (StringFromGUID2(interfaces[i], iidStr, ARRAYSIZE(iidStr)) == 0) {
            hr = E_UNEXPECTED;
            break;
        }
        swprintf_s(valueName, L"APOInterface%d", i);
        hr = WriteRegSz(key, valueName, iidStr);
    }

    RegCloseKey(key);
    return hr;
}

} // namespace

// ---------------------------------------------------------------------------
// DLL exports

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppvObject)
{
#if defined(KWIET_DEV_LOG)
    char g1[40], g2[40];
    KWIET_LOG("DllGetClassObject: clsid=%s riid=%s",
              KwietGuidToA(rclsid, g1, sizeof(g1)), KwietGuidToA(riid, g2, sizeof(g2)));
#endif
    if (ppvObject == nullptr) {
        return E_POINTER;
    }
    *ppvObject = nullptr;

    if (!IsEqualCLSID(rclsid, CLSID_KwietApo)) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    KwietClassFactory* factory = new (std::nothrow) KwietClassFactory();
    if (factory == nullptr) {
        return E_OUTOFMEMORY;
    }
    const HRESULT hr = factory->QueryInterface(riid, ppvObject);
    factory->Release();
    return hr;
}

STDAPI DllCanUnloadNow()
{
    return g_moduleRefs.load(std::memory_order_acquire) == 0 ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer()
{
    wchar_t dllPath[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(g_kwietModule, dllPath, ARRAYSIZE(dllPath));
    if (len == 0 || len >= ARRAYSIZE(dllPath)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    wchar_t keyPath[128] = {};
    HRESULT hr = BuildClsidKeyPath(keyPath, ARRAYSIZE(keyPath));
    if (FAILED(hr)) {
        return hr;
    }

    HKEY clsidKey = nullptr;
    LSTATUS st = RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyPath, 0, nullptr, 0,
                                 KEY_WRITE | KEY_WOW64_64KEY, nullptr, &clsidKey, nullptr);
    if (st != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(st);
    }

    hr = WriteRegSz(clsidKey, nullptr, L"Kwiet Passthrough APO");

    HKEY inprocKey = nullptr;
    if (SUCCEEDED(hr)) {
        st = RegCreateKeyExW(clsidKey, L"InprocServer32", 0, nullptr, 0,
                             KEY_WRITE | KEY_WOW64_64KEY, nullptr, &inprocKey, nullptr);
        hr = HRESULT_FROM_WIN32(st);
    }
    if (SUCCEEDED(hr)) {
        hr = WriteRegSz(inprocKey, nullptr, dllPath);
    }
    if (SUCCEEDED(hr)) {
        // audiodg instantiates APOs from its multithreaded apartment.
        hr = WriteRegSz(inprocKey, L"ThreadingModel", L"Both");
    }

    if (inprocKey != nullptr) {
        RegCloseKey(inprocKey);
    }
    RegCloseKey(clsidKey);

    if (SUCCEEDED(hr)) {
        hr = RegisterApoCatalog();
    }
    return hr;
}

STDAPI DllUnregisterServer()
{
    wchar_t keyPath[160] = {};
    HRESULT hr = BuildClsidKeyPath(keyPath, ARRAYSIZE(keyPath));
    if (FAILED(hr)) {
        return hr;
    }

    LSTATUS st = RegDeleteTreeW(HKEY_LOCAL_MACHINE, keyPath);
    if (st != ERROR_SUCCESS && st != ERROR_FILE_NOT_FOUND) {
        return HRESULT_FROM_WIN32(st);
    }

    hr = BuildApoCatalogKeyPath(keyPath, ARRAYSIZE(keyPath));
    if (FAILED(hr)) {
        return hr;
    }
    st = RegDeleteTreeW(HKEY_LOCAL_MACHINE, keyPath);
    if (st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND) {
        return S_OK;
    }
    return HRESULT_FROM_WIN32(st);
}

BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID /*lpReserved*/)
{
    if (dwReason == DLL_PROCESS_ATTACH) {
        g_kwietModule = reinterpret_cast<HMODULE>(hInstance);
        DisableThreadLibraryCalls(hInstance);
#if defined(KWIET_DEV_LOG)
        // No file I/O under loader lock: debugger channel only.
        OutputDebugStringA("KwietApo: DLL_PROCESS_ATTACH\n");
#endif
    }
    return TRUE;
}
