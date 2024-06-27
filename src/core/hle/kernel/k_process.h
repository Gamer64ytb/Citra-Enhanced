// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>
#include <boost/container/static_vector.hpp>
#include <boost/serialization/export.hpp>
#include "common/bit_field.h"
#include "common/common_types.h"
#include "core/hle/kernel/k_auto_object.h"
#include "core/hle/kernel/k_code_set.h"
#include "core/hle/kernel/k_handle_table.h"
#include "core/hle/kernel/slab_helpers.h"
#include "core/hle/kernel/vm_manager.h"

namespace Kernel {

struct AddressMapping {
    // Address and size must be page-aligned
    VAddr address;
    u32 size;
    bool read_only;
    bool unk_flag;

private:
    friend class boost::serialization::access;
    template <class Archive>
    void serialize(Archive& ar, const unsigned int);
};

union ProcessFlags {
    u16 raw;

    BitField<0, 1, u16>
        allow_debug; ///< Allows other processes to attach to and debug this process.
    BitField<1, 1, u16> force_debug; ///< Allows this process to attach to processes even if they
                                     /// don't have allow_debug set.
    BitField<2, 1, u16> allow_nonalphanum;
    BitField<3, 1, u16> shared_page_writable; ///< Shared page is mapped with write permissions.
    BitField<4, 1, u16> privileged_priority;  ///< Can use priority levels higher than 24.
    BitField<5, 1, u16> allow_main_args;
    BitField<6, 1, u16> shared_device_mem;
    BitField<7, 1, u16> runnable_on_sleep;
    BitField<8, 4, MemoryRegion>
        memory_region;                ///< Default region for memory allocations for this process
    BitField<12, 1, u16> loaded_high; ///< Application loaded high (not at 0x00100000).
};

enum class ProcessStatus {
    Created,
    Running,
    Exited,
};

class KResourceLimit;
enum class ResourceLimitType : u32;
struct MemoryRegionInfo;

class Process final : public KAutoObjectWithSlabHeapAndContainer<Process> {
    KERNEL_AUTOOBJECT_TRAITS(Process, KAutoObject);

public:
    explicit Process(Kernel::KernelSystem& kernel);
    ~Process() override;

    KHandleTable handle_table;

    CodeSet codeset{};
    /// Resource limit descriptor for this process
    KResourceLimit* resource_limit{};

    /// The process may only call SVCs which have the corresponding bit set.
    std::bitset<0x80> svc_access_mask;
    /// Maximum size of the handle table for the process.
    u32 handle_table_size = 0x200;
    /// Special memory ranges mapped into this processes address space. This is used to give
    /// processes access to specific I/O regions and device memory.
    boost::container::static_vector<AddressMapping, 8> address_mappings;
    ProcessFlags flags{};
    bool no_thread_restrictions = false;
    /// Kernel compatibility version for this process
    u16 kernel_version = 0;
    /// The default CPU for this process, threads are scheduled on this cpu by default.
    u8 ideal_processor = 0;
    /// Current status of the process
    ProcessStatus status;

    /// The id of this process
    u32 process_id;

    // Creation time in ticks of the process.
    u64 creation_time_ticks;

    void Initialize();

    static void PostDestroy(uintptr_t arg) {}

    void Finalize() override;

    /**
     * Parses a list of kernel capability descriptors (as found in the ExHeader) and applies them
     * to this process.
     */
    void ParseKernelCaps(const u32* kernel_caps, std::size_t len);

    /**
     * Set up the default kernel capability for 3DSX.
     */
    void Set3dsxKernelCaps();

    /**
     * Applies address space changes and launches the process main thread.
     */
    void Run(s32 main_thread_priority, u32 stack_size);

    /**
     * Called when the process exits by svc
     */
    void Exit();

    VMManager vm_manager;

    u32 memory_used = 0;

    std::shared_ptr<MemoryRegionInfo> memory_region = nullptr;
    MemoryRegionInfo::IntervalSet holding_memory;
    MemoryRegionInfo::IntervalSet holding_tls_memory;

    /// The Thread Local Storage area is allocated as processes create threads,
    /// each TLS area is 0x200 bytes, so one page (0x1000) is split up in 8 parts, and each part
    /// holds the TLS for a specific thread. This vector contains which parts are in use for each
    /// page as a bitmask.
    /// This vector will grow as more pages are allocated for new threads.
    std::vector<std::bitset<8>> tls_slots;

    VAddr GetLinearHeapAreaAddress() const;
    VAddr GetLinearHeapBase() const;
    VAddr GetLinearHeapLimit() const;

    Result HeapAllocate(VAddr* out_addr, VAddr target, u32 size, VMAPermission perms,
                        MemoryState memory_state = MemoryState::Private,
                        bool skip_range_check = false);
    Result HeapFree(VAddr target, u32 size);

    Result LinearAllocate(VAddr* out_addr, VAddr target, u32 size, VMAPermission perms);
    Result LinearFree(VAddr target, u32 size);

    Result AllocateThreadLocalStorage(VAddr* out_tls);

    Result Map(VAddr target, VAddr source, u32 size, VMAPermission perms, bool privileged = false);
    Result Unmap(VAddr target, VAddr source, u32 size, VMAPermission perms,
                 bool privileged = false);

    void ReleaseResource(ResourceLimitType type, s32 amount);

private:
    void FreeAllMemory();

    KernelSystem& kernel;

    friend class boost::serialization::access;
    template <class Archive>
    void serialize(Archive& ar, const unsigned int file_version);
};

} // namespace Kernel

BOOST_CLASS_EXPORT_KEY(Kernel::Process)
CONSTRUCT_KERNEL_OBJECT(Kernel::Process)
