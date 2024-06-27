// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <boost/serialization/base_object.hpp>
#include "common/archives.h"
#include "common/logging/log.h"
#include "core/hle/kernel/errors.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_resource_limit.h"
#include "core/hle/kernel/k_shared_memory.h"
#include "core/hle/kernel/memory.h"
#include "core/memory.h"

SERIALIZE_EXPORT_IMPL(Kernel::KSharedMemory)

namespace Kernel {

KSharedMemory::KSharedMemory(KernelSystem& kernel) : KAutoObjectWithSlabHeapAndContainer(kernel) {}

KSharedMemory::~KSharedMemory() = default;

Result KSharedMemory::Initialize(Process* owner, u32 size, MemoryPermission permissions,
                                 MemoryPermission other_permissions, VAddr address,
                                 MemoryRegion region) {
    // Open a reference to our owner process.
    if (owner) {
        owner->Open();
        m_owner = owner;
    }

    // Set member variables.
    m_base_address = address;
    m_size = size;
    m_permissions = permissions;
    m_other_permissions = other_permissions;

    // Allocate the shared memory block.
    if (address == 0) {
        // We need to allocate a block from the Linear Heap ourselves.
        // We'll manually allocate some memory from the linear heap in the specified region.
        auto memory_region = m_kernel.GetMemoryRegion(region);
        auto offset = memory_region->LinearAllocate(size);
        ASSERT_MSG(offset, "Not enough space in region to allocate shared memory!");

        // Store the backing blocks of allocated memory.
        auto& memory = m_kernel.memory;
        std::fill(memory.GetFCRAMPointer(*offset), memory.GetFCRAMPointer(*offset + size), 0);
        m_backing_blocks = {{memory.GetFCRAMRef(*offset), size}};
        m_holding_memory += MemoryRegionInfo::Interval(*offset, *offset + size);
        m_linear_heap_phys_offset = *offset;

        // Increase the amount of used linear heap memory for the owner process.
        if (m_owner) {
            m_owner->memory_used += size;
        }
    } else {
        // The memory is already available and mapped in the owner process.
        ASSERT(m_owner);
        auto& vm_manager = m_owner->vm_manager;
        R_TRY(vm_manager.ChangeMemoryState(address, size, MemoryState::Private,
                                           VMAPermission::ReadWrite, MemoryState::Locked,
                                           KSharedMemory::ConvertPermissions(permissions)));

        // Should succeed after verifying memory state above.
        auto backing_blocks = vm_manager.GetBackingBlocksForRange(address, size);
        ASSERT(backing_blocks.Succeeded());
        m_backing_blocks = std::move(backing_blocks).Unwrap();
    }

    return ResultSuccess;
}

void KSharedMemory::InitializeForApplet(u32 offset, u32 size, MemoryPermission permissions,
                                        MemoryPermission other_permissions) {
    // Allocate memory in heap
    auto memory_region = m_kernel.GetMemoryRegion(MemoryRegion::SYSTEM);
    auto backing_blocks = memory_region->HeapAllocate(size);
    ASSERT_MSG(!backing_blocks.empty(), "Not enough space in region to allocate shared memory!");

    // Set member variables
    m_holding_memory = backing_blocks;
    m_base_address = Memory::HEAP_VADDR + offset;
    m_size = size;
    m_permissions = permissions;
    m_other_permissions = other_permissions;

    // Initialize backing blocks
    auto& memory = m_kernel.memory;
    for (const auto& interval : backing_blocks) {
        const VAddr addr = interval.lower();
        const VAddr end = interval.upper();
        m_backing_blocks.emplace_back(memory.GetFCRAMRef(addr), end - addr);
        std::fill(memory.GetFCRAMPointer(addr), memory.GetFCRAMPointer(end), 0);
    }
}

void KSharedMemory::Finalize() {
    auto memory_region = m_kernel.GetMemoryRegion(MemoryRegion::SYSTEM);
    for (const auto& interval : m_holding_memory) {
        memory_region->Free(interval.lower(), interval.upper() - interval.lower());
    }

    if (m_owner) {
        if (m_base_address != 0) {
            m_owner->vm_manager.ChangeMemoryState(m_base_address, m_size, MemoryState::Locked,
                                                  VMAPermission::None, MemoryState::Private,
                                                  VMAPermission::ReadWrite);
        } else {
            m_owner->memory_used -= m_size;
        }
    }
}

void KSharedMemory::PostDestroy(uintptr_t arg) {
    Process* owner = reinterpret_cast<Process*>(arg);
    if (owner != nullptr) {
        owner->ReleaseResource(ResourceLimitType::SharedMemory, 1);
        owner->Close();
    }
}

Result KSharedMemory::Map(Process& target_process, VAddr address, MemoryPermission permissions,
                          MemoryPermission other_permissions) {

    const MemoryPermission own_other_permissions =
        &target_process == m_owner ? m_permissions : m_other_permissions;

    // Automatically allocated memory blocks can only be mapped with other_permissions = DontCare
    R_UNLESS(m_base_address != 0 || other_permissions == MemoryPermission::DontCare,
             ResultInvalidCombination);

    // Heap-backed memory blocks can not be mapped with other_permissions = DontCare
    R_UNLESS(m_base_address == 0 || other_permissions != MemoryPermission::DontCare,
             ResultInvalidCombination);

    // Error out if the requested permissions don't match what the creator process allows.
    if (static_cast<u32>(permissions) & ~static_cast<u32>(own_other_permissions)) {
        LOG_ERROR(Kernel, "cannot map address={:#08X}, permissions don't match", address);
        return ResultInvalidCombination;
    }

    // Error out if the provided permissions are not compatible with what the creator process needs.
    if (other_permissions != MemoryPermission::DontCare &&
        static_cast<u32>(m_permissions) & ~static_cast<u32>(other_permissions)) {
        LOG_ERROR(Kernel, "cannot map address={:#08X}, permissions don't match", address);
        return ResultWrongPermission;
    }

    // TODO(Subv): Check for the Shared Device Mem flag in the creator process.
    /*if (was_created_with_shared_device_mem && address != 0) {
        return Result(ErrorDescription::InvalidCombination, ErrorModule::OS,
    ErrorSummary::InvalidArgument, ErrorLevel::Usage);
    }*/

    // TODO(Subv): The same process that created a SharedMemory object
    // can not map it in its own address space unless it was created with addr=0, result 0xD900182C.

    if (address != 0) {
        if (address < Memory::HEAP_VADDR || address + m_size >= Memory::SHARED_MEMORY_VADDR_END) {
            LOG_ERROR(Kernel, "cannot map address={:#08X}, invalid address", address);
            return ResultInvalidAddress;
        }
    }

    VAddr target_address = address;
    if (m_base_address == 0 && target_address == 0) {
        // Calculate the address at which to map the memory block.
        // Note: even on new firmware versions, the target address is still in the old linear heap
        // region. This exception is made to keep the shared font compatibility. See
        // APT:GetSharedFont for detail.
        target_address = m_linear_heap_phys_offset + Memory::LINEAR_HEAP_VADDR;
    }
    {
        auto vma = target_process.vm_manager.FindVMA(target_address);
        if (vma->second.type != VMAType::Free ||
            vma->second.base + vma->second.size < target_address + m_size) {
            LOG_ERROR(Kernel, "cannot map address={:#08X}, mapping to already allocated memory",
                      address);
            return ResultInvalidAddressState;
        }
    }

    // Map the memory block into the target process
    VAddr interval_target = target_address;
    for (const auto& interval : m_backing_blocks) {
        auto vma = target_process.vm_manager.MapBackingMemory(interval_target, interval.first,
                                                              interval.second, MemoryState::Shared);
        ASSERT(vma.Succeeded());
        target_process.vm_manager.Reprotect(vma.Unwrap(), ConvertPermissions(permissions));
        interval_target += interval.second;
    }

    return ResultSuccess;
}

Result KSharedMemory::Unmap(Process& target_process, VAddr address) {
    // TODO(Subv): Verify what happens if the application tries to unmap an address that is not
    // mapped to a SharedMemory.
    return target_process.vm_manager.UnmapRange(address, m_size);
}

VMAPermission KSharedMemory::ConvertPermissions(MemoryPermission permission) {
    u32 masked_permissions =
        static_cast<u32>(permission) & static_cast<u32>(MemoryPermission::ReadWriteExecute);
    return static_cast<VMAPermission>(masked_permissions);
};

u8* KSharedMemory::GetPointer(u32 offset) {
    if (m_backing_blocks.size() != 1) {
        LOG_WARNING(Kernel, "Unsafe GetPointer on discontinuous SharedMemory");
    }
    return m_backing_blocks[0].first + offset;
}

const u8* KSharedMemory::GetPointer(u32 offset) const {
    if (m_backing_blocks.size() != 1) {
        LOG_WARNING(Kernel, "Unsafe GetPointer on discontinuous SharedMemory");
    }
    return m_backing_blocks[0].first + offset;
}

template <class Archive>
void KSharedMemory::serialize(Archive& ar, const u32 file_version) {
    ar& boost::serialization::base_object<KAutoObject>(*this);
    ar& m_linear_heap_phys_offset;
    // ar& m_backing_blocks;
    ar& m_size;
    ar& m_permissions;
    ar& m_other_permissions;
    ar& m_owner;
    ar& m_base_address;
    ar& m_holding_memory;
}

SERIALIZE_IMPL(KSharedMemory)

} // namespace Kernel
