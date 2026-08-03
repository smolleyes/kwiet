#pragma once

#include <windows.h>

#include "KwietControl.h"

// Owns the shared-memory control block described in KwietControl.h.
//
// The APO is the creator, not the UI: creating an object in the `Global\`
// namespace needs SeCreateGlobalPrivilege, which a normal interactive process
// does not have. audiodg does. The UI therefore opens what we create, and
// re-pushes its stored settings whenever `generation` changes.
//
// Open/Close are non-RT (they call into the kernel). Once open, the block is
// just memory: reading and writing it from the real-time path is fine.
class ControlShm final
{
public:
    ControlShm() = default;

    ControlShm(const ControlShm&) = delete;
    ControlShm& operator=(const ControlShm&) = delete;

    ~ControlShm()
    {
        Close();
    }

    // Creates the block, or opens it if another APO instance got there first.
    // Non-RT. Returns false when the control plane is unavailable; the APO then
    // runs on its built-in defaults, which is a valid degraded mode.
    bool Open();

    void Close();

    // Null until Open() succeeds. Stable while open.
    KwietControlBlock* Block() const
    {
        return m_block;
    }

private:
    HANDLE m_mapping = nullptr;
    KwietControlBlock* m_block = nullptr;
};
