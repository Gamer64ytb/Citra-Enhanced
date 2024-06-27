// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "common/common_types.h"
#include "core/hle/kernel/k_client_port.h"
#include "core/hle/kernel/k_server_port.h"
#include "core/hle/kernel/slab_helpers.h"
#include "core/hle/result.h"

namespace Kernel {

class KServerSession;

class KPort final : public KAutoObjectWithSlabHeapAndContainer<KPort> {
    KERNEL_AUTOOBJECT_TRAITS(KPort, KAutoObject);

public:
    explicit KPort(KernelSystem& kernel);
    ~KPort() override;

    static void PostDestroy(uintptr_t arg) {}

    void Initialize(s32 max_sessions, std::string name);
    void OnClientClosed();
    void OnServerClosed();

    bool IsServerClosed() const;

    Result EnqueueSession(KServerSession* session);

    KClientPort& GetClientPort() {
        return m_client;
    }
    KServerPort& GetServerPort() {
        return m_server;
    }
    const KClientPort& GetClientPort() const {
        return m_client;
    }
    const KServerPort& GetServerPort() const {
        return m_server;
    }

private:
    KServerPort m_server;
    KClientPort m_client;
};

} // namespace Kernel
