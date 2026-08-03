#include "ControlShm.h"

#include <sddl.h>

#include "KwietDevLog.h"

namespace {

// Who may touch the control block:
//   SY  LOCAL SYSTEM            full
//   BA  Administrators          full
//   AU  Authenticated Users     query + map read + map write
// plus a Medium mandatory label, without which the default High label would
// stop a normal (medium integrity) UI process from writing.
//
// THREAT MODEL: any medium-integrity process in the user's session can toggle
// the effect and change the aggressiveness. That is a nuisance, not an
// escalation -- no pointer, size or length crosses this boundary, and every
// value is clamped on read. Tighten to INTERACTIVE or a dedicated group if the
// product ever needs it.
constexpr wchar_t kSddl[] = L"D:(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x0007;;;AU)S:(ML;;NW;;;ME)";

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

    if (m_mapping == nullptr) {
        KWIET_LOG("ControlShm: CreateFileMapping failed, err=%lu", createError);
        return false;
    }
    const bool alreadyExisted = (createError == ERROR_ALREADY_EXISTS);

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
