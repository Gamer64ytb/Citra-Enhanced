// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <boost/serialization/export.hpp>
#include <boost/serialization/shared_ptr.hpp>
#include <boost/serialization/string.hpp>
#include "common/archives.h"
#include "common/assert.h"
#include "core/hle/kernel/errors.h"
#include "core/hle/kernel/hle_ipc.h"
#include "core/hle/kernel/k_client_port.h"
#include "core/hle/kernel/k_client_session.h"
#include "core/hle/kernel/k_port.h"
#include "core/hle/kernel/k_server_port.h"
#include "core/hle/kernel/k_session.h"

SERIALIZE_EXPORT_IMPL(Kernel::KClientPort)

namespace Kernel {

KClientPort::KClientPort(KernelSystem& kernel) : KAutoObject(kernel) {}

KClientPort::~KClientPort() = default;

void KClientPort::Initialize(KPort* parent, s32 max_sessions, std::string name) {
    // Set member variables.
    m_parent = parent;
    m_max_sessions = max_sessions;
    m_name = name + "_Client";
}

Result KClientPort::CreateSession(KClientSession** out) {
    R_UNLESS(m_active_sessions < m_max_sessions, ResultMaxConnectionsReached);
    m_active_sessions++;

    // Allocate a new session.
    KSession* session = KSession::Create(m_kernel);

    // Initialize the session.
    session->Initialize(this);

    // Register the session.
    KSession::Register(m_kernel, session);

    // Let the created sessions inherit the parent port's HLE handler.
    auto* server = &m_parent->GetServerPort();
    auto hle_handler = server->GetHleHandler();
    if (hle_handler) {
        hle_handler->ClientConnected(&session->GetServerSession());
    } else {
        server->EnqueueSession(&session->GetServerSession());
    }

    // Wake the threads waiting on the ServerPort
    m_parent->GetServerPort().WakeupAllWaitingThreads();

    // We succeeded, so set the output.
    *out = std::addressof(session->GetClientSession());
    return ResultSuccess;
}

void KClientPort::ConnectionClosed() {
    ASSERT(m_active_sessions > 0);
    --m_active_sessions;
}

template <class Archive>
void KClientPort::serialize(Archive& ar, const u32 file_version) {
    ar& boost::serialization::base_object<KAutoObject>(*this);
    // ar& m_parent;
    ar& m_max_sessions;
    ar& m_active_sessions;
    ar& m_name;
}

SERIALIZE_IMPL(KClientPort)

} // namespace Kernel
