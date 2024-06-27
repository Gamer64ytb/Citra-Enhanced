// Copyright 2019 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/scope_exit.h"
#include "core/hle/kernel/ipc_debugger/recorder.h"
#include "core/hle/kernel/k_client_port.h"
#include "core/hle/kernel/k_client_session.h"
#include "core/hle/kernel/k_port.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_server_port.h"
#include "core/hle/kernel/k_server_session.h"
#include "core/hle/kernel/k_session.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/service/service.h"

namespace IPCDebugger {

namespace {

ObjectInfo GetObjectInfo(const Kernel::KAutoObject* object) {
    if (object == nullptr) {
        return {};
    }
    return {object->GetTypeName(), /*object->GetName()*/ "KAutoObject",
            /*static_cast<int>(object->GetObjectId())*/ 1};
}

ObjectInfo GetObjectInfo(const Kernel::KThread* thread) {
    if (thread == nullptr) {
        return {};
    }
    return {thread->GetTypeName(), /*thread->GetName()*/ "KThread",
            /*static_cast<int>(object->GetObjectId())*/ 1};
}

ObjectInfo GetObjectInfo(const Kernel::Process* process) {
    if (process == nullptr) {
        return {};
    }
    return {process->GetTypeName(), /*process->GetName()*/ "KProcess",
            static_cast<int>(process->process_id)};
}

} // Anonymous namespace

Recorder::Recorder() = default;

Recorder::~Recorder() = default;

bool Recorder::IsEnabled() const {
    return enabled.load(std::memory_order_relaxed);
}

void Recorder::RegisterRequest(const Kernel::KClientSession* client_session,
                               const Kernel::KThread* client_thread) {
    const u32 thread_id = client_thread->GetThreadId();
    const RequestRecord record = {
        .id = ++record_count,
        .status = RequestStatus::Sent,
        .client_process = GetObjectInfo(client_thread->GetOwner()),
        .client_thread = GetObjectInfo(client_thread),
        .client_session = GetObjectInfo(client_session),
        .client_port = GetObjectInfo(client_session->GetParent()->GetParent()),
        .server_process = {},
        .server_thread = {},
        .server_session = GetObjectInfo(&client_session->GetParent()->GetServerSession()),
    };

    record_map.insert_or_assign(thread_id, std::make_unique<RequestRecord>(record));
    client_session_map.insert_or_assign(thread_id, client_session);
    InvokeCallbacks(record);
}

void Recorder::SetRequestInfo(const Kernel::KThread* client_thread,
                              std::vector<u32> untranslated_cmdbuf,
                              std::vector<u32> translated_cmdbuf,
                              const Kernel::KThread* server_thread) {
    const u32 thread_id = client_thread->GetThreadId();
    if (!record_map.count(thread_id)) {
        // This is possible when the recorder is enabled after application started
        LOG_ERROR(Kernel, "No request is associated with the thread");
        return;
    }

    auto& record = *record_map[thread_id];
    record.status = RequestStatus::Handling;
    record.untranslated_request_cmdbuf = std::move(untranslated_cmdbuf);
    record.translated_request_cmdbuf = std::move(translated_cmdbuf);

    if (server_thread) {
        record.server_process = GetObjectInfo(server_thread->GetOwner());
        record.server_thread = GetObjectInfo(server_thread);
    } else {
        record.is_hle = true;
    }

    // Function name
    ASSERT_MSG(client_session_map.count(thread_id), "Client session is missing");
    const auto client_session = client_session_map[thread_id];

    SCOPE_EXIT({
        client_session_map.erase(thread_id);
        InvokeCallbacks(record);
    });

    auto port = client_session->GetParent()->GetParent();
    if (!port) {
        return;
    }

    auto hle_handler = port->GetParent()->GetServerPort().GetHleHandler();
    if (hle_handler) {
        record.function_name = std::dynamic_pointer_cast<Service::ServiceFrameworkBase>(hle_handler)
                                   ->GetFunctionName({record.untranslated_request_cmdbuf[0]});
    }
}

void Recorder::SetReplyInfo(const Kernel::KThread* client_thread,
                            std::vector<u32> untranslated_cmdbuf,
                            std::vector<u32> translated_cmdbuf) {
    const u32 thread_id = client_thread->GetThreadId();
    if (!record_map.count(thread_id)) {
        // This is possible when the recorder is enabled after application started
        LOG_ERROR(Kernel, "No request is associated with the thread");
        return;
    }

    auto& record = *record_map[thread_id];
    if (record.status != RequestStatus::HLEUnimplemented) {
        record.status = RequestStatus::Handled;
    }

    record.untranslated_reply_cmdbuf = std::move(untranslated_cmdbuf);
    record.translated_reply_cmdbuf = std::move(translated_cmdbuf);
    InvokeCallbacks(record);

    record_map.erase(thread_id);
}

void Recorder::SetHLEUnimplemented(const Kernel::KThread* client_thread) {
    const u32 thread_id = client_thread->GetThreadId();
    if (!record_map.count(thread_id)) {
        // This is possible when the recorder is enabled after application started
        LOG_ERROR(Kernel, "No request is associated with the thread");
        return;
    }

    auto& record = *record_map[thread_id];
    record.status = RequestStatus::HLEUnimplemented;
}

CallbackHandle Recorder::BindCallback(CallbackType callback) {
    std::unique_lock lock(callback_mutex);
    CallbackHandle handle = std::make_shared<CallbackType>(callback);
    callbacks.emplace(handle);
    return handle;
}

void Recorder::UnbindCallback(const CallbackHandle& handle) {
    std::unique_lock lock(callback_mutex);
    callbacks.erase(handle);
}

void Recorder::InvokeCallbacks(const RequestRecord& request) {
    {
        std::shared_lock lock(callback_mutex);
        for (const auto& iter : callbacks) {
            (*iter)(request);
        }
    }
}

void Recorder::SetEnabled(bool enabled_) {
    enabled.store(enabled_, std::memory_order_relaxed);
}

} // namespace IPCDebugger
