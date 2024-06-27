// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_mutex.h"
#include "core/hle/kernel/k_session.h"
#include "core/hle/kernel/k_shared_memory.h"
#include "core/hle/service/kernel_helpers.h"

namespace Service::KernelHelpers {

ServiceContext::ServiceContext(Core::System& system_) : kernel{system_.Kernel()} {}

ServiceContext::~ServiceContext() {
    // Close references to tracked objects.
    for (Kernel::KAutoObject* object : objects) {
        object->Close();
    }
}

Kernel::KEvent* ServiceContext::CreateEvent(Kernel::ResetType reset_type, std::string&& name) {
    // Create a new object.
    auto* event = Kernel::KEvent::Create(kernel);
    if (!event) {
        LOG_CRITICAL(Service, "Unable to create object!");
        return {};
    }

    // Initialize the object.
    event->Initialize(nullptr, reset_type);

    // Register the object.
    Kernel::KEvent::Register(kernel, event);
    objects.push_back(event);
    return event;
}

Kernel::KMutex* ServiceContext::CreateMutex(bool initial_locked, std::string&& name) {
    // Create a new object.
    auto* mutex = Kernel::KMutex::Create(kernel);
    if (!mutex) {
        LOG_CRITICAL(Service, "Unable to create object!");
        return {};
    }

    // Initialize the object.
    mutex->Initialize(nullptr, initial_locked);

    // Register the object.
    Kernel::KMutex::Register(kernel, mutex);
    objects.push_back(mutex);
    return mutex;
}

Kernel::KSession* ServiceContext::CreateSession(std::string&& name) {
    // Create a new object.
    auto* session = Kernel::KSession::Create(kernel);
    if (!session) {
        LOG_CRITICAL(Service, "Unable to create object!");
        return {};
    }

    // Initialize the object.
    session->Initialize(nullptr);

    // Register the object.
    Kernel::KSession::Register(kernel, session);
    objects.push_back(session);
    return session;
}

Kernel::KSharedMemory* ServiceContext::CreateSharedMemory(
    u32 size, Kernel::MemoryPermission permissions, Kernel::MemoryPermission other_permissions,
    VAddr address, Kernel::MemoryRegion region, std::string&& name) {
    // Create a new object.
    Kernel::KSharedMemory* shared_mem = Kernel::KSharedMemory::Create(kernel);
    if (!shared_mem) {
        LOG_CRITICAL(Service, "Unable to create object!");
        return {};
    }

    // Initialize the object.
    shared_mem->Initialize(nullptr, size, permissions, other_permissions, address, region);

    // Register the object.
    Kernel::KSharedMemory::Register(kernel, shared_mem);
    objects.push_back(shared_mem);
    return shared_mem;
}

Kernel::KSharedMemory* ServiceContext::CreateSharedMemoryForApplet(
    u32 offset, u32 size, Kernel::MemoryPermission permissions,
    Kernel::MemoryPermission other_permissions, std::string&& name) {
    // Create a new object.
    Kernel::KSharedMemory* shared_mem = Kernel::KSharedMemory::Create(kernel);
    if (!shared_mem) {
        LOG_CRITICAL(Service, "Unable to create object!");
        return {};
    }

    // Initialize the object.
    shared_mem->InitializeForApplet(offset, size, permissions, other_permissions);

    // Register the object.
    Kernel::KSharedMemory::Register(kernel, shared_mem);
    objects.push_back(shared_mem);
    return shared_mem;
}

} // namespace Service::KernelHelpers
