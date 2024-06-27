// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <string>
#include <type_traits>
#include <vector>

#include "core/hle/kernel/k_auto_object.h"

namespace Core {
class System;
}

namespace Kernel {
class KSharedMemory;
class KernelSystem;
class KEvent;
class KMutex;
class KSession;
class Process;
enum class MemoryRegion : u16;
enum class MemoryPermission : u32;
enum class ResetType : u32;
} // namespace Kernel

namespace Service::KernelHelpers {

template <typename T>
concept IsKAutoObject = std::is_base_of_v<Kernel::KAutoObject, T>;

class ServiceContext {
public:
    explicit ServiceContext(Core::System& system_);
    ~ServiceContext();

    Kernel::KEvent* CreateEvent(Kernel::ResetType reset_type, std::string&& name);

    Kernel::KMutex* CreateMutex(bool initial_locked, std::string&& name);

    Kernel::KSession* CreateSession(std::string&& name);

    Kernel::KSharedMemory* CreateSharedMemory(u32 size, Kernel::MemoryPermission permissions,
                                              Kernel::MemoryPermission other_permissions,
                                              VAddr address, Kernel::MemoryRegion region,
                                              std::string&& name);

    Kernel::KSharedMemory* CreateSharedMemoryForApplet(u32 offset, u32 size,
                                                       Kernel::MemoryPermission permissions,
                                                       Kernel::MemoryPermission other_permissions,
                                                       std::string&& name);

private:
    Kernel::KernelSystem& kernel;
    std::vector<Kernel::KAutoObject*> objects;
};

} // namespace Service::KernelHelpers
