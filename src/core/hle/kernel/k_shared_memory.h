// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <boost/serialization/export.hpp>
#include "common/common_types.h"
#include "core/global.h"
#include "core/hle/kernel/slab_helpers.h"
#include "core/hle/result.h"

namespace Kernel {

enum class VMAPermission : u8;

class KSharedMemory final : public KAutoObjectWithSlabHeapAndContainer<KSharedMemory> {
    KERNEL_AUTOOBJECT_TRAITS(KSharedMemory, KAutoObject);

public:
    explicit KSharedMemory(KernelSystem& kernel);
    ~KSharedMemory() override;

    Result Initialize(Process* owner, u32 size, MemoryPermission permissions,
                      MemoryPermission other_permissions, VAddr address, MemoryRegion region);

    void InitializeForApplet(u32 offset, u32 size, MemoryPermission permissions,
                             MemoryPermission other_permissions);

    void Finalize() override;

    uintptr_t GetPostDestroyArgument() const override {
        return reinterpret_cast<uintptr_t>(m_owner);
    }

    static void PostDestroy(uintptr_t arg);

    Process* GetOwner() const override {
        return m_owner;
    }

    u64 GetSize() const {
        return m_size;
    }

    u64 GetLinearHeapPhysicalOffset() const {
        return m_linear_heap_phys_offset;
    }

    void SetName(std::string&& name_) {}

    /**
     * Converts the specified MemoryPermission into the equivalent VMAPermission.
     * @param permission The MemoryPermission to convert.
     */
    static VMAPermission ConvertPermissions(MemoryPermission permission);

    /**
     * Maps a shared memory block to an address in the target process' address space
     * @param target_process Process on which to map the memory block.
     * @param address Address in system memory to map shared memory block to
     * @param permissions Memory block map permissions (specified by SVC field)
     * @param other_permissions Memory block map other permissions (specified by SVC field)
     */
    Result Map(Process& target_process, VAddr address, MemoryPermission permissions,
               MemoryPermission other_permissions);

    /**
     * Unmaps a shared memory block from the specified address in system memory
     * @param target_process Process from which to unmap the memory block.
     * @param address Address in system memory where the shared memory block is mapped
     * @return Result code of the unmap operation
     */
    Result Unmap(Process& target_process, VAddr address);

    /**
     * Gets a pointer to the shared memory block
     * @param offset Offset from the start of the shared memory block to get pointer
     * @return A pointer to the shared memory block from the specified offset
     */
    u8* GetPointer(u32 offset = 0);

    /**
     * Gets a constant pointer to the shared memory block
     * @param offset Offset from the start of the shared memory block to get pointer
     * @return A constant pointer to the shared memory block from the specified offset
     */
    const u8* GetPointer(u32 offset = 0) const;

private:
    friend class boost::serialization::access;
    template <class Archive>
    void serialize(Archive& ar, const u32 file_version);

private:
    Process* m_owner{};
    PAddr m_linear_heap_phys_offset{};
    VAddr m_base_address{};
    u32 m_size{};
    MemoryPermission m_permissions{};
    MemoryPermission m_other_permissions{};
    std::vector<std::pair<MemoryRef, u32>> m_backing_blocks;
    MemoryRegionInfo::IntervalSet m_holding_memory;
};

} // namespace Kernel

BOOST_CLASS_EXPORT_KEY(Kernel::KSharedMemory)
CONSTRUCT_KERNEL_OBJECT(Kernel::KSharedMemory)
