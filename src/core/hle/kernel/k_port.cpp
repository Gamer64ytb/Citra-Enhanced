// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "core/hle/kernel/k_port.h"

namespace Kernel {

KPort::KPort(KernelSystem& kernel)
    : KAutoObjectWithSlabHeapAndContainer{kernel}, m_server{kernel}, m_client{kernel} {}

KPort::~KPort() = default;

void KPort::Initialize(s32 max_sessions, std::string name) {
    // Open a new reference count to the initialized port.
    this->Open();

    // Create and initialize our server/client pair.
    KAutoObject::Create(std::addressof(m_server));
    KAutoObject::Create(std::addressof(m_client));
    m_server.Initialize(this, name);
    m_client.Initialize(this, max_sessions, name);
}

} // namespace Kernel
