// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <boost/serialization/base_object.hpp>
#include <boost/serialization/shared_ptr.hpp>
#include <boost/serialization/string.hpp>
#include <boost/serialization/vector.hpp>
#include "common/archives.h"
#include "common/assert.h"
#include "core/hle/kernel/k_port.h"
#include "core/hle/kernel/k_server_port.h"
#include "core/hle/kernel/k_server_session.h"
#include "core/hle/kernel/k_thread.h"

SERIALIZE_EXPORT_IMPL(Kernel::KServerPort)

namespace Kernel {

KServerPort::KServerPort(KernelSystem& kernel) : KSynchronizationObject(kernel) {}

KServerPort::~KServerPort() = default;

void KServerPort::Initialize(KPort* parent, std::string name) {
    m_parent = parent;
    m_name = name + "_Server";
}

void KServerPort::Destroy() {
    // Close our reference to our parent.
    m_parent->Close();
}

KServerSession* KServerPort::AcceptSession() {
    // Return the first session in the list.
    if (m_pending_sessions.empty()) {
        return nullptr;
    }

    KServerSession* session = m_pending_sessions.back();
    m_pending_sessions.pop_back();
    return session;
}

void KServerPort::EnqueueSession(KServerSession* session) {
    // Add the session to our queue.
    m_pending_sessions.push_back(session);
}

bool KServerPort::ShouldWait(const KThread* thread) const {
    // If there are no pending sessions, we wait until a new one is added.
    return m_pending_sessions.size() == 0;
}

void KServerPort::Acquire(KThread* thread) {
    ASSERT_MSG(!ShouldWait(thread), "object unavailable!");
}

template <class Archive>
void KServerPort::serialize(Archive& ar, const u32 file_version) {
    ar& boost::serialization::base_object<KSynchronizationObject>(*this);
    ar& m_name;
    ar& m_pending_sessions;
    // ar& m_hle_handler;
}
SERIALIZE_IMPL(KServerPort)

} // namespace Kernel
