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
        if (ppvObject == nullptr) {
            return E_POINTER;
        }
        *ppvObject = nullptr;
        if (pUnkOuter != nullptr) {
            return CLASS_E_NOAGGREGATION;
        }

        KwietApo* apo = new (std::nothrow) KwietApo();
        if (apo == nullptr) {
            return E_OUTOFMEMORY;
        }
        const HRESULT hr = apo->QueryInterface(riid, ppvObject);
        apo->Release();
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

} // namespace

// ---------------------------------------------------------------------------
// DLL exports

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppvObject)
{
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
    return hr;
}

STDAPI DllUnregisterServer()
{
    wchar_t keyPath[128] = {};
    const HRESULT hr = BuildClsidKeyPath(keyPath, ARRAYSIZE(keyPath));
    if (FAILED(hr)) {
        return hr;
    }

    const LSTATUS st = RegDeleteTreeW(HKEY_LOCAL_MACHINE, keyPath);
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
    }
    return TRUE;
}
