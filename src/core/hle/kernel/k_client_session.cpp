// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <boost/serialization/base_object.hpp>
#include <boost/serialization/string.hpp>
#include "common/archives.h"
#include "core/hle/kernel/k_auto_object_container.h"
#include "core/hle/kernel/k_client_session.h"
#include "core/hle/kernel/k_server_session.h"
#include "core/hle/kernel/k_session.h"
#include "core/hle/kernel/k_thread.h"

SERIALIZE_EXPORT_IMPL(Kernel::KClientSession)

namespace Kernel {

KClientSession::KClientSession(KernelSystem& kernel) : KAutoObject(kernel) {}

KClientSession::~KClientSession() = default;

void KClientSession::Destroy() {
    m_parent->OnClientClosed();
    m_parent->Close();
}

void KClientSession::OnServerClosed() {}

Result KClientSession::SendSyncRequest(KThread* thread) {
    // Signal the server session that new data is available
    return m_parent->GetServerSession().HandleSyncRequest(thread);
}

template <class Archive>
void KClientSession::serialize(Archive& ar, const u32 file_version) {
    ar& boost::serialization::base_object<KAutoObject>(*this);
    // ar& m_parent;
}

SERIALIZE_IMPL(KClientSession)

} // namespace Kernel
