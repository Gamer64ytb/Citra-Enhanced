// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <string>

#include "core/hle/kernel/k_client_session.h"
#include "core/hle/kernel/k_server_session.h"
#include "core/hle/kernel/slab_helpers.h"

namespace Kernel {

class KClientPort;

enum class KSessionState : u8 {
    Invalid = 0,
    Normal = 1,
    ClientClosed = 2,
    ServerClosed = 3,
};

class KSession final : public KAutoObjectWithSlabHeapAndContainer<KSession> {
    KERNEL_AUTOOBJECT_TRAITS(KSession, KAutoObject);

public:
    explicit KSession(KernelSystem& kernel);
    ~KSession() override;

    void Initialize(KClientPort* port);
    void Finalize() override;

    bool IsInitialized() const override {
        return m_initialized;
    }

    static void PostDestroy(uintptr_t arg) {}

    void OnServerClosed();

    void OnClientClosed();

    KSessionState GetState() const {
        return m_state;
    }

    KClientSession& GetClientSession() {
        return m_client;
    }

    KServerSession& GetServerSession() {
        return m_server;
    }

    const KClientSession& GetClientSession() const {
        return m_client;
    }

    const KServerSession& GetServerSession() const {
        return m_server;
    }

    KClientPort* GetParent() {
        return m_port;
    }

private:
    KServerSession m_server;
    KClientSession m_client;
    KClientPort* m_port{};
    KSessionState m_state{};
    bool m_initialized{};
};

} // namespace Kernel
