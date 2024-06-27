// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <boost/serialization/shared_ptr.hpp>
#include <boost/serialization/string.hpp>
#include <boost/serialization/vector.hpp>
#include "common/archives.h"
#include "core/hle/kernel/hle_ipc.h"
#include "core/hle/kernel/k_client_session.h"
#include "core/hle/kernel/k_server_session.h"
#include "core/hle/kernel/k_session.h"
#include "core/hle/kernel/k_thread.h"

SERIALIZE_EXPORT_IMPL(Kernel::KServerSession)

namespace Kernel {

KServerSession::KServerSession(KernelSystem& kernel) : KSynchronizationObject(kernel) {}

KServerSession::~KServerSession() = default;

void KServerSession::Destroy() {
    m_parent->OnServerClosed();
    m_parent->Close();
}

bool KServerSession::ShouldWait(const KThread* thread) const {
    // Closed sessions should never wait, an error will be returned from svcReplyAndReceive.
    const auto state = m_parent->GetState();
    if (state != KSessionState::Normal) {
        return false;
    }
    // Wait if we have no pending requests, or if we're currently handling a request.
    return pending_requesting_threads.empty() || currently_handling != nullptr;
}

void KServerSession::Acquire(KThread* thread) {
    ASSERT_MSG(!ShouldWait(thread), "object unavailable!");

    // If the client endpoint was closed, don't do anything. This KServerSession is now useless and
    // will linger until its last handle is closed by the running application.
    const auto state = m_parent->GetState();
    if (state != KSessionState::Normal) {
        return;
    }

    // We are now handling a request, pop it from the stack.
    ASSERT(!pending_requesting_threads.empty());
    currently_handling = pending_requesting_threads.back();
    pending_requesting_threads.pop_back();
}

void KServerSession::OnClientClosed() {
    // Notify HLE handler that client session has been disconnected.
    if (hle_handler) {
        hle_handler->ClientDisconnected(this);
    }

    // Clean up the list of client threads with pending requests, they are unneeded now that the
    // client endpoint is closed.
    pending_requesting_threads.clear();
    currently_handling = nullptr;

    // Notify any threads waiting on the KServerSession that the endpoint has been closed. Note
    // that this call has to happen after `Session::client` has been set to nullptr to let the
    // ServerSession know that the client endpoint has been closed.
    this->WakeupAllWaitingThreads();
}

Result KServerSession::HandleSyncRequest(KThread* thread) {
    // The KServerSession received a sync request, this means that there's new data available
    // from its ClientSession, so wake up any threads that may be waiting on a svcReplyAndReceive or
    // similar.

    // If this KServerSession has an associated HLE handler, forward the request to it.
    if (hle_handler != nullptr) {
        std::array<u32_le, IPC::COMMAND_BUFFER_LENGTH + 2 * IPC::MAX_STATIC_BUFFERS> cmd_buf;
        auto current_process = thread->GetOwner();
        ASSERT(current_process);
        m_kernel.memory.ReadBlock(*current_process, thread->GetCommandBufferAddress(),
                                  cmd_buf.data(), cmd_buf.size() * sizeof(u32));

        auto context = std::make_shared<Kernel::HLERequestContext>(m_kernel, this, thread);
        context->PopulateFromIncomingCommandBuffer(cmd_buf.data(), current_process);

        hle_handler->HandleSyncRequest(*context);

        ASSERT(thread->m_status == Kernel::ThreadStatus::Running ||
               thread->m_status == Kernel::ThreadStatus::WaitHleEvent);
        // Only write the response immediately if the thread is still running. If the HLE handler
        // put the thread to sleep then the writing of the command buffer will be deferred to the
        // wakeup callback.
        if (thread->m_status == Kernel::ThreadStatus::Running) {
            context->WriteToOutgoingCommandBuffer(cmd_buf.data(), *current_process);
            m_kernel.memory.WriteBlock(*current_process, thread->GetCommandBufferAddress(),
                                       cmd_buf.data(), cmd_buf.size() * sizeof(u32));
        }
    }

    if (thread->m_status == ThreadStatus::Running) {
        // Put the thread to sleep until the server replies, it will be awoken in
        // svcReplyAndReceive for LLE servers.
        thread->m_status = ThreadStatus::WaitIPC;

        if (hle_handler != nullptr) {
            // For HLE services, we put the request threads to sleep for a short duration to
            // simulate IPC overhead, but only if the HLE handler didn't put the thread to sleep for
            // other reasons like an async callback. The IPC overhead is needed to prevent
            // starvation when a thread only does sync requests to HLE services while a
            // lower-priority thread is waiting to run.

            // This delay was approximated in a homebrew application by measuring the average time
            // it takes for svcSendSyncRequest to return when performing the SetLcdForceBlack IPC
            // request to the GSP:GPU service in a n3DS with firmware 11.6. The measured values have
            // a high variance and vary between models.
            static constexpr u64 IPCDelayNanoseconds = 39000;
            thread->WakeAfterDelay(IPCDelayNanoseconds);
        } else {
            // Add the thread to the list of threads that have issued a sync request with this
            // server.
            pending_requesting_threads.push_back(std::move(thread));
        }
    }

    // If this KServerSession does not have an HLE implementation,
    // just wake up the threads waiting on it.
    this->WakeupAllWaitingThreads();
    return ResultSuccess;
}

template <class Archive>
void KServerSession::serialize(Archive& ar, const u32 file_version) {
    ar& boost::serialization::base_object<KSynchronizationObject>(*this);
    ar& m_name;
    // ar& m_parent;
    ar& hle_handler;
    ar& pending_requesting_threads;
    ar& currently_handling;
    ar& mapped_buffer_context;
}
SERIALIZE_IMPL(KServerSession)

} // namespace Kernel
