#include "ControlShm.h"

#include <sddl.h>

#include "KwietDevLog.h"

namespace {

// Who may touch the control block:
//   SY  LOCAL SYSTEM            full
//   BA  Administrators          full
//   LS  LOCAL SERVICE           full   (audiodg runs as a service account)
//   NS  NETWORK SERVICE         full
//   AU  Authenticated Users     SECTION_ALL_ACCESS
// plus a Medium mandatory label, without which the default High label would
// stop a normal (medium integrity) UI process from writing.
//
// SECTION_ALL_ACCESS rather than just map-read/map-write, because whoever gets
// here second has to *open* the object, and CreateFileMapping asks for full
// section access when the name already exists. With a narrower grant, the APO
// fails with ERROR_ACCESS_DENIED as soon as the UI holds the section open --
// which is exactly what the UI does to keep settings alive between streams.
// The looser right costs nothing: anyone who can map the page read-write
// already controls its contents entirely.
//
// THREAT MODEL: any medium-integrity process in the user's session can toggle
// the effect and change the aggressiveness. That is a nuisance, not an
// escalation -- no pointer, size or length crosses this boundary, and every
// value is clamped on read. Tighten to INTERACTIVE or a dedicated group if the
// product ever needs it.
constexpr wchar_t kSddl[] =
    L"D:(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;LS)(A;;FA;;;NS)(A;;0xF001F;;;AU)S:(ML;;NW;;;ME)";

} // namespace

bool ControlShm::Open()
{
    Close();

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            kSddl, SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr)) {
        KWIET_LOG("ControlShm: bad SDDL, err=%lu", GetLastError());
        return false;
    }

    m_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0,
                                   sizeof(KwietControlBlock), KWIET_CONTROL_NAME);
    const DWORD createError = GetLastError();
    LocalFree(sa.lpSecurityDescriptor);

    bool alreadyExisted = (createError == ERROR_ALREADY_EXISTS);

    if (m_mapping == nullptr) {
        KWIET_LOG("ControlShm: CreateFileMapping failed, err=%lu", createError);
        // A section left over from an older build carries the old, too narrow
        // ACL, so create-or-open is refused. Opening with only the rights we
        // actually need still works, and the stale section disappears once its
        // last holder exits.
        m_mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, KWIET_CONTROL_NAME);
        if (m_mapping == nullptr) {
            KWIET_LOG("ControlShm: OpenFileMapping fallback failed, err=%lu", GetLastError());
            return false;
        }
        // Reached only when the section was already there, so it must NOT be
        // re-initialised: the UI's settings live in it.
        alreadyExisted = true;
        KWIET_LOG("ControlShm: joined a pre-existing section through the fallback");
    }

    m_block = static_cast<KwietControlBlock*>(
        MapViewOfFile(m_mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(KwietControlBlock)));
    if (m_block == nullptr) {
        KWIET_LOG("ControlShm: MapViewOfFile failed, err=%lu", GetLastError());
        Close();
        return false;
    }

    if (!alreadyExisted) {
        // Fresh section: the kernel zeroed it, so only the non-zero defaults
        // need writing. magic goes last -- it is what tells the UI the rest is
        // meaningful.
        m_block->version.store(KWIET_CONTROL_VERSION, std::memory_order_relaxed);
        m_block->enabled.store(1, std::memory_order_relaxed);
        m_block->aggressivenessTenths.store(KWIET_AGGRESSIVENESS_DEFAULT_TENTHS,
                                            std::memory_order_relaxed);
        m_block->magic.store(KWIET_CONTROL_MAGIC, std::memory_order_release);
        KWIET_LOG("ControlShm: created %S", KWIET_CONTROL_NAME);
    } else {
        // Another APO instance (the other processing mode) owns it already.
        // Refuse to share a block written by a different layout.
        const uint32_t version = m_block->version.load(std::memory_order_acquire);
        if (m_block->magic.load(std::memory_order_acquire) != KWIET_CONTROL_MAGIC
            || version != KWIET_CONTROL_VERSION) {
            KWIET_LOG("ControlShm: version mismatch (found %u, expected %u)", version,
                      KWIET_CONTROL_VERSION);
            Close();
            return false;
        }
        KWIET_LOG("ControlShm: joined existing block");
    }
    return true;
}

void ControlShm::Close()
{
    if (m_block != nullptr) {
        UnmapViewOfFile(m_block);
        m_block = nullptr;
    }
    if (m_mapping != nullptr) {
        CloseHandle(m_mapping);
        m_mapping = nullptr;
    }
}
