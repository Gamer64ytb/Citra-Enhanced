// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/kernel/k_client_port.h"
#include "core/hle/kernel/k_client_session.h"
#include "core/hle/kernel/k_server_session.h"
#include "core/hle/kernel/k_session.h"

namespace Kernel {

KSession::KSession(KernelSystem& kernel)
    : KAutoObjectWithSlabHeapAndContainer{kernel}, m_server{kernel}, m_client{kernel} {}

KSession::~KSession() = default;

void KSession::Initialize(KClientPort* client_port) {
    // Increment reference count.
    // Because reference count is one on creation, this will result
    // in a reference count of two. Thus, when both server and client are closed
    // this object will be destroyed.
    this->Open();

    // Create our sub sessions.
    KAutoObject::Create(std::addressof(m_server));
    KAutoObject::Create(std::addressof(m_client));

    // Initialize our sub sessions.
    m_state = KSessionState::Normal;
    m_server.Initialize(this);
    m_client.Initialize(this);

    // Set our port.
    m_port = client_port;
    if (m_port != nullptr) {
        m_port->Open();
    }

    // Mark initialized.
    m_initialized = true;
}

void KSession::Finalize() {
    if (m_port != nullptr) {
        m_port->ConnectionClosed();
        m_port->Close();
    }
}

void KSession::OnServerClosed() {
    if (m_state == KSessionState::Normal) {
        m_state = KSessionState::ServerClosed;
        m_client.OnServerClosed();
    }
}

void KSession::OnClientClosed() {
    if (m_state == KSessionState::Normal) {
        m_state = KSessionState::ClientClosed;
        m_server.OnClientClosed();
    }
}

} // namespace Kernel
