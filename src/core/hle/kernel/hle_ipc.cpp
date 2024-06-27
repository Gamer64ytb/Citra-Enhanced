// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <vector>
#include <boost/serialization/assume_abstract.hpp>
#include <boost/serialization/shared_ptr.hpp>
#include <boost/serialization/unique_ptr.hpp>
#include <boost/serialization/vector.hpp>
#include "common/archives.h"
#include "common/assert.h"
#include "common/common_types.h"
#include "core/core.h"
#include "core/hle/kernel/hle_ipc.h"
#include "core/hle/kernel/ipc_debugger/recorder.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_handle_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"

SERIALIZE_EXPORT_IMPL(Kernel::SessionRequestHandler)
SERIALIZE_EXPORT_IMPL(Kernel::SessionRequestHandler::SessionDataBase)
SERIALIZE_EXPORT_IMPL(Kernel::SessionRequestHandler::SessionInfo)
SERIALIZE_EXPORT_IMPL(Kernel::HLERequestContext)
SERIALIZE_EXPORT_IMPL(Kernel::HLERequestContext::ThreadCallback)

namespace Kernel {

class HLERequestContext::ThreadCallback : public Kernel::WakeupCallback {

public:
    ThreadCallback(std::shared_ptr<HLERequestContext> context_,
                   std::shared_ptr<HLERequestContext::WakeupCallback> callback_)
        : callback(std::move(callback_)), context(std::move(context_)) {}
    void WakeUp(ThreadWakeupReason reason, KThread* thread, KSynchronizationObject* object) {
        ASSERT(thread->m_status == ThreadStatus::WaitHleEvent);
        if (callback) {
            callback->WakeUp(thread, *context, reason);
        }

        Process* process = thread->GetOwner();

        // We must copy the entire command buffer *plus* the entire static buffers area, since
        // the translation might need to read from it in order to retrieve the StaticBuffer
        // target addresses.
        std::array<u32_le, IPC::COMMAND_BUFFER_LENGTH + 2 * IPC::MAX_STATIC_BUFFERS> cmd_buff;
        Memory::MemorySystem& memory = context->kernel.memory;
        memory.ReadBlock(*process, thread->GetCommandBufferAddress(), cmd_buff.data(),
                         cmd_buff.size() * sizeof(u32));
        context->WriteToOutgoingCommandBuffer(cmd_buff.data(), *process);
        // Copy the translated command buffer back into the thread's command buffer area.
        memory.WriteBlock(*process, thread->GetCommandBufferAddress(), cmd_buff.data(),
                          cmd_buff.size() * sizeof(u32));
    }

private:
    ThreadCallback() = default;
    std::shared_ptr<HLERequestContext::WakeupCallback> callback{};
    std::shared_ptr<HLERequestContext> context{};

    template <class Archive>
    void serialize(Archive& ar, const unsigned int) {
        ar& boost::serialization::base_object<Kernel::WakeupCallback>(*this);
        ar& callback;
        ar& context;
    }
    friend class boost::serialization::access;
};

SessionRequestHandler::SessionInfo::SessionInfo(KServerSession* session_,
                                                std::unique_ptr<SessionDataBase> data)
    : session(session_), data(std::move(data)) {}

void SessionRequestHandler::ClientConnected(KServerSession* server_session) {
    server_session->SetHleHandler(shared_from_this());
    connected_sessions.emplace_back(server_session, MakeSessionData());
}

void SessionRequestHandler::ClientDisconnected(KServerSession* server_session) {
    server_session->SetHleHandler(nullptr);
    connected_sessions.erase(
        std::remove_if(connected_sessions.begin(), connected_sessions.end(),
                       [&](const SessionInfo& info) { return info.session == server_session; }),
        connected_sessions.end());
}

template <class Archive>
void SessionRequestHandler::serialize(Archive& ar, const unsigned int) {
    ar& connected_sessions;
}
SERIALIZE_IMPL(SessionRequestHandler)

template <class Archive>
void SessionRequestHandler::SessionDataBase::serialize(Archive& ar, const unsigned int) {}
SERIALIZE_IMPL(SessionRequestHandler::SessionDataBase)

template <class Archive>
void SessionRequestHandler::SessionInfo::serialize(Archive& ar, const unsigned int) {
    ar& session;
    ar& data;
}
SERIALIZE_IMPL(SessionRequestHandler::SessionInfo)

KEvent* HLERequestContext::SleepClientThread(const std::string& reason,
                                             std::chrono::nanoseconds timeout,
                                             std::shared_ptr<WakeupCallback> callback) {
    // Put the client thread to sleep until the wait event is signaled or the timeout expires.
    thread->m_wakeup_callback = std::make_shared<ThreadCallback>(shared_from_this(), callback);

    // Create pause event.
    auto* event = KEvent::Create(kernel);
    event->Initialize(nullptr, ResetType::OneShot);
    event->SetName("HLE Pause Event: " + reason);
    KEvent::Register(kernel, event);

    // Add the event to the list of objects the thread is waiting for.
    thread->m_status = ThreadStatus::WaitHleEvent;
    thread->m_wait_objects = {event};
    event->AddWaitingThread(thread);

    if (timeout.count() > 0) {
        thread->WakeAfterDelay(timeout.count());
    }

    return event;
}

HLERequestContext::HLERequestContext() : kernel(Core::Global<KernelSystem>()) {}

HLERequestContext::HLERequestContext(KernelSystem& kernel, KServerSession* session, KThread* thread)
    : kernel(kernel), session(session), thread(thread) {
    cmd_buf[0] = 0;
}

HLERequestContext::~HLERequestContext() = default;

KAutoObject* HLERequestContext::GetIncomingHandle(u32 id_from_cmdbuf) const {
    ASSERT(id_from_cmdbuf < request_handles.size());
    return request_handles[id_from_cmdbuf];
}

u32 HLERequestContext::AddOutgoingHandle(KAutoObject* object) {
    request_handles.push_back(object);
    return static_cast<u32>(request_handles.size() - 1);
}

void HLERequestContext::ClearIncomingObjects() {
    request_handles.clear();
}

const std::vector<u8>& HLERequestContext::GetStaticBuffer(u8 buffer_id) const {
    return static_buffers[buffer_id];
}

void HLERequestContext::AddStaticBuffer(u8 buffer_id, std::vector<u8> data) {
    static_buffers[buffer_id] = std::move(data);
}

Result HLERequestContext::PopulateFromIncomingCommandBuffer(const u32_le* src_cmdbuf,
                                                            Process* src_process) {
    IPC::Header header{src_cmdbuf[0]};

    std::size_t untranslated_size = 1u + header.normal_params_size;
    std::size_t command_size = untranslated_size + header.translate_params_size;
    ASSERT(command_size <= IPC::COMMAND_BUFFER_LENGTH); // TODO(yuriks): Return error

    std::copy_n(src_cmdbuf, untranslated_size, cmd_buf.begin());

    const bool should_record = kernel.GetIPCRecorder().IsEnabled();

    std::vector<u32> untranslated_cmdbuf;
    if (should_record) {
        untranslated_cmdbuf = std::vector<u32>{src_cmdbuf, src_cmdbuf + command_size};
    }

    std::size_t i = untranslated_size;
    while (i < command_size) {
        u32 descriptor = cmd_buf[i] = src_cmdbuf[i];
        i += 1;

        switch (IPC::GetDescriptorType(descriptor)) {
        case IPC::DescriptorType::CopyHandle:
        case IPC::DescriptorType::MoveHandle: {
            const u32 num_handles = IPC::HandleNumberFromDesc(descriptor);
            auto& src_handle_table = src_process->handle_table;
            ASSERT(i + num_handles <= command_size); // TODO(yuriks): Return error
            for (u32 j = 0; j < num_handles; ++j) {
                const Handle handle = src_cmdbuf[i];
                if (!handle) {
                    cmd_buf[i++] = AddOutgoingHandle(nullptr);
                    continue;
                }

                // Get object from the handle table.
                KScopedAutoObject object =
                    src_handle_table.GetObjectForIpcWithoutPseudoHandle(handle);
                ASSERT(object.IsNotNull());

                // If we are moving, remove the old handle.
                if (descriptor == IPC::DescriptorType::MoveHandle) {
                    src_handle_table.Remove(handle);
                }

                cmd_buf[i++] = AddOutgoingHandle(object.GetPointerUnsafe());
            }
            break;
        }
        case IPC::DescriptorType::CallingPid: {
            cmd_buf[i++] = src_process->process_id;
            break;
        }
        case IPC::DescriptorType::StaticBuffer: {
            VAddr source_address = src_cmdbuf[i];
            IPC::StaticBufferDescInfo buffer_info{descriptor};

            // Copy the input buffer into our own vector and store it.
            std::vector<u8> data(buffer_info.size);
            kernel.memory.ReadBlock(*src_process, source_address, data.data(), data.size());

            AddStaticBuffer(buffer_info.buffer_id, std::move(data));
            cmd_buf[i++] = source_address;
            break;
        }
        case IPC::DescriptorType::MappedBuffer: {
            u32 next_id = static_cast<u32>(request_mapped_buffers.size());
            request_mapped_buffers.emplace_back(kernel.memory, src_process, descriptor,
                                                src_cmdbuf[i], next_id);
            cmd_buf[i++] = next_id;
            break;
        }
        default:
            UNIMPLEMENTED_MSG("Unsupported handle translation: {:#010X}", descriptor);
        }
    }

    if (should_record) {
        std::vector<u32> translated_cmdbuf{cmd_buf.begin(), cmd_buf.begin() + command_size};
        kernel.GetIPCRecorder().SetRequestInfo(thread, std::move(untranslated_cmdbuf),
                                               std::move(translated_cmdbuf));
    }

    return ResultSuccess;
}

Result HLERequestContext::WriteToOutgoingCommandBuffer(u32_le* dst_cmdbuf,
                                                       Process& dst_process) const {
    IPC::Header header{cmd_buf[0]};

    std::size_t untranslated_size = 1u + header.normal_params_size;
    std::size_t command_size = untranslated_size + header.translate_params_size;
    ASSERT(command_size <= IPC::COMMAND_BUFFER_LENGTH);

    std::copy_n(cmd_buf.begin(), untranslated_size, dst_cmdbuf);

    const bool should_record = kernel.GetIPCRecorder().IsEnabled();

    std::vector<u32> untranslated_cmdbuf;
    if (should_record) {
        untranslated_cmdbuf = std::vector<u32>{cmd_buf.begin(), cmd_buf.begin() + command_size};
    }

    std::size_t i = untranslated_size;
    while (i < command_size) {
        u32 descriptor = dst_cmdbuf[i] = cmd_buf[i];
        i += 1;

        switch (IPC::GetDescriptorType(descriptor)) {
        case IPC::DescriptorType::CopyHandle:
        case IPC::DescriptorType::MoveHandle: {
            // HLE services don't use handles, so we treat both CopyHandle and MoveHandle equally
            const u32 num_handles = IPC::HandleNumberFromDesc(descriptor);
            ASSERT(i + num_handles <= command_size);
            for (u32 j = 0; j < num_handles; ++j) {
                KAutoObject* object = GetIncomingHandle(cmd_buf[i]);
                Handle handle = 0;
                if (object != nullptr) {
                    dst_process.handle_table.Add(std::addressof(handle), object);
                }
                dst_cmdbuf[i++] = handle;
            }
            break;
        }
        case IPC::DescriptorType::StaticBuffer: {
            IPC::StaticBufferDescInfo buffer_info{descriptor};

            const auto& data = GetStaticBuffer(buffer_info.buffer_id);

            // Grab the address that the target thread set up to receive the response static buffer
            // and write our data there. The static buffers area is located right after the command
            // buffer area.
            std::size_t static_buffer_offset =
                IPC::COMMAND_BUFFER_LENGTH + 2 * buffer_info.buffer_id;
            IPC::StaticBufferDescInfo target_descriptor{dst_cmdbuf[static_buffer_offset]};
            VAddr target_address = dst_cmdbuf[static_buffer_offset + 1];

            ASSERT_MSG(target_descriptor.size >= data.size(), "Static buffer data is too big");

            kernel.memory.WriteBlock(dst_process, target_address, data.data(), data.size());

            dst_cmdbuf[i++] = target_address;
            break;
        }
        case IPC::DescriptorType::MappedBuffer: {
            VAddr addr = request_mapped_buffers[cmd_buf[i]].address;
            dst_cmdbuf[i++] = addr;
            break;
        }
        default:
            UNIMPLEMENTED_MSG("Unsupported handle translation: {:#010X}", descriptor);
        }
    }

    if (should_record) {
        std::vector<u32> translated_cmdbuf{dst_cmdbuf, dst_cmdbuf + command_size};
        kernel.GetIPCRecorder().SetReplyInfo(thread, std::move(untranslated_cmdbuf),
                                             std::move(translated_cmdbuf));
    }

    return ResultSuccess;
}

MappedBuffer& HLERequestContext::GetMappedBuffer(u32 id_from_cmdbuf) {
    ASSERT_MSG(id_from_cmdbuf < request_mapped_buffers.size(), "Mapped Buffer ID out of range!");
    return request_mapped_buffers[id_from_cmdbuf];
}

void HLERequestContext::ReportUnimplemented() const {
    if (kernel.GetIPCRecorder().IsEnabled()) {
        kernel.GetIPCRecorder().SetHLEUnimplemented(thread);
    }
}

template <class Archive>
void HLERequestContext::serialize(Archive& ar, const unsigned int) {
    ar& cmd_buf;
    ar& session;
    ar& thread;
    // ar& request_handles;
    ar& static_buffers;
    ar& request_mapped_buffers;
}
SERIALIZE_IMPL(HLERequestContext)

MappedBuffer::MappedBuffer() : memory(&Core::Global<Core::System>().Memory()) {}

MappedBuffer::MappedBuffer(Memory::MemorySystem& memory, Process* process, u32 descriptor,
                           VAddr address, u32 id)
    : memory(&memory), id(id), address(address), process(std::move(process)) {
    IPC::MappedBufferDescInfo desc{descriptor};
    size = desc.size;
    perms = desc.perms;
}

void MappedBuffer::Read(void* dest_buffer, std::size_t offset, std::size_t size) {
    ASSERT(perms & IPC::R);
    ASSERT(offset + size <= this->size);
    memory->ReadBlock(*process, address + static_cast<VAddr>(offset), dest_buffer, size);
}

void MappedBuffer::Write(const void* src_buffer, std::size_t offset, std::size_t size) {
    ASSERT(perms & IPC::W);
    ASSERT(offset + size <= this->size);
    memory->WriteBlock(*process, address + static_cast<VAddr>(offset), src_buffer, size);
}

} // namespace Kernel
