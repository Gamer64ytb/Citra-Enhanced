// Copyright 2014 Citra Emulator Project / PPSSPP Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <vector>
#include "common/bit_field.h"
#include "common/common_types.h"
#include "core/hle/kernel/memory.h"
#include "core/memory.h"

namespace ConfigMem {
class Handler;
}

namespace SharedPage {
class Handler;
}

namespace Memory {
class MemorySystem;
}

namespace Core {
class ARM_Interface;
class Timing;
} // namespace Core

namespace IPCDebugger {
class Recorder;
}

namespace Kernel {

class CodeSet;
class Process;
class KThread;
class ResourceLimitList;
class SharedMemory;
class ThreadManager;
class TimerManager;
class VMManager;
struct AddressMapping;
class KAutoObject;
class KObjectName;
class KObjectNameGlobalData;

/// Permissions for mapped shared memory blocks
enum class MemoryPermission : u32 {
    None = 0,
    Read = (1u << 0),
    Write = (1u << 1),
    ReadWrite = (Read | Write),
    Execute = (1u << 2),
    ReadExecute = (Read | Execute),
    WriteExecute = (Write | Execute),
    ReadWriteExecute = (Read | Write | Execute),
    DontCare = (1u << 28)
};
DECLARE_ENUM_FLAG_OPERATORS(MemoryPermission)

enum class MemoryRegion : u16 {
    APPLICATION = 1,
    SYSTEM = 2,
    BASE = 3,
};

union CoreVersion {
    CoreVersion(u32 version) : raw(version) {}
    CoreVersion(u32 major_ver, u32 minor_ver, u32 revision_ver) {
        revision.Assign(revision_ver);
        minor.Assign(minor_ver);
        major.Assign(major_ver);
    }

    u32 raw = 0;
    BitField<8, 8, u32> revision;
    BitField<16, 8, u32> minor;
    BitField<24, 8, u32> major;
};

/// Common memory memory modes.
enum class MemoryMode : u8 {
    Prod = 0, ///< 64MB app memory
    Dev1 = 2, ///< 96MB app memory
    Dev2 = 3, ///< 80MB app memory
    Dev3 = 4, ///< 72MB app memory
    Dev4 = 5, ///< 32MB app memory
};

/// New 3DS memory modes.
enum class New3dsMemoryMode : u8 {
    Legacy = 0,  ///< Use Old 3DS system mode.
    NewProd = 1, ///< 124MB app memory
    NewDev1 = 2, ///< 178MB app memory
    NewDev2 = 3, ///< 124MB app memory
};

/// Structure containing N3DS hardware capability flags.
struct New3dsHwCapabilities {
    bool enable_l2_cache;         ///< Whether extra L2 cache should be enabled.
    bool enable_804MHz_cpu;       ///< Whether the CPU should run at 804MHz.
    New3dsMemoryMode memory_mode; ///< The New 3DS memory mode.

private:
    template <class Archive>
    void serialize(Archive& ar, const unsigned int);
    friend class boost::serialization::access;
};

template <typename T>
class KSlabHeap;
class KAutoObjectWithListContainer;

class KernelSystem {
public:
    explicit KernelSystem(Memory::MemorySystem& memory, Core::Timing& timing,
                          std::function<void()> prepare_reschedule_callback, MemoryMode memory_mode,
                          u32 num_cores, const New3dsHwCapabilities& n3ds_hw_caps,
                          u64 override_init_time = 0);
    ~KernelSystem();

    /**
     * Terminates a process, killing its threads and removing it from the process list.
     * @param process Process to terminate.
     */
    void TerminateProcess(Process* process);

    ResourceLimitList& ResourceLimit();
    const ResourceLimitList& ResourceLimit() const;

    u32 GenerateObjectID();

    /// Gets the slab heap for the specified kernel object type.
    template <typename T>
    KSlabHeap<T>& SlabHeap();

    KAutoObjectWithListContainer& ObjectListContainer();

    /// Gets global data for KObjectName.
    KObjectNameGlobalData& ObjectNameGlobalData();

    /// Registers all kernel objects with the global emulation state, this is purely for tracking
    /// leaks after emulation has been shutdown.
    void RegisterKernelObject(KAutoObject* object);

    /// Unregisters a kernel object previously registered with RegisterKernelObject when it was
    /// destroyed during the current emulation session.
    void UnregisterKernelObject(KAutoObject* object);

    /// Retrieves a process from the current list of processes.
    Process* GetProcessById(u32 process_id) const;

    const std::vector<Process*>& GetProcessList() const {
        return process_list;
    }

    Process* GetCurrentProcess() const;
    void SetCurrentProcess(Process* process);
    void SetCurrentProcessForCPU(Process* process, u32 core_id);

    void SetCurrentMemoryPageTable(std::shared_ptr<Memory::PageTable> page_table);

    void SetCPUs(std::vector<std::shared_ptr<Core::ARM_Interface>> cpu);

    void SetRunningCPU(Core::ARM_Interface* cpu);

    ThreadManager& GetThreadManager(u32 core_id);
    const ThreadManager& GetThreadManager(u32 core_id) const;

    ThreadManager& GetCurrentThreadManager();
    const ThreadManager& GetCurrentThreadManager() const;

    TimerManager& GetTimerManager();
    const TimerManager& GetTimerManager() const;

    void MapSharedPages(VMManager& address_space);

    SharedPage::Handler& GetSharedPageHandler();
    const SharedPage::Handler& GetSharedPageHandler() const;

    ConfigMem::Handler& GetConfigMemHandler();

    IPCDebugger::Recorder& GetIPCRecorder();
    const IPCDebugger::Recorder& GetIPCRecorder() const;

    std::shared_ptr<MemoryRegionInfo> GetMemoryRegion(MemoryRegion region);

    void HandleSpecialMapping(VMManager& address_space, const AddressMapping& mapping);

    std::array<std::shared_ptr<MemoryRegionInfo>, 3> memory_regions{};

    void PrepareReschedule() {
        prepare_reschedule_callback();
    }

    u32 NewThreadId();
    u32 NewProcessId();

    void ResetThreadIDs();

    MemoryMode GetMemoryMode() const {
        return memory_mode;
    }

    const New3dsHwCapabilities& GetNew3dsHwCapabilities() const {
        return n3ds_hw_caps;
    }

    std::recursive_mutex& GetHLELock() {
        return hle_lock;
    }

    Core::ARM_Interface* current_cpu = nullptr;

    Memory::MemorySystem& memory;

    Core::Timing& timing;

    // Lists all processes that exist in the current session.
    std::vector<Process*> process_list;

    /// Sleep main thread of the first ever launched non-sysmodule process.
    void SetAppMainThreadExtendedSleep(bool requires_sleep) {
        main_thread_extended_sleep = requires_sleep;
    }

    bool GetAppMainThreadExtendedSleep() const {
        return main_thread_extended_sleep;
    }

private:
    void MemoryInit(MemoryMode memory_mode, New3dsMemoryMode n3ds_mode, u64 override_init_time);

    std::function<void()> prepare_reschedule_callback;

    std::unique_ptr<ResourceLimitList> resource_limits;
    std::atomic<u32> next_object_id{0};

    // Note: keep the member order below in order to perform correct destruction.
    // Thread manager is destructed before process list in order to Stop threads and clear thread
    // info from their parent processes first. Timer manager is destructed after process list
    // because timers are destructed along with process list and they need to clear info from the
    // timer manager.
    // TODO (wwylele): refactor the cleanup sequence to make this less complicated and sensitive.

    std::unique_ptr<TimerManager> timer_manager;

    // TODO(Subv): Start the process ids from 10 for now, as lower PIDs are
    // reserved for low-level services
    u32 next_process_id = 10;

    Process* current_process{};
    std::vector<Process*> stored_processes;

    std::vector<std::unique_ptr<ThreadManager>> thread_managers;

    std::shared_ptr<ConfigMem::Handler> config_mem_handler;
    std::shared_ptr<SharedPage::Handler> shared_page_handler;

    std::unique_ptr<IPCDebugger::Recorder> ipc_recorder;

    u32 next_thread_id;

    MemoryMode memory_mode;
    New3dsHwCapabilities n3ds_hw_caps;

    /// Helper to encapsulate all slab heaps in a single heap allocated container
    struct SlabHeapContainer;
    std::unique_ptr<SlabHeapContainer> slab_heap_container;

    std::unique_ptr<KObjectNameGlobalData> object_name_global_data;

    std::unique_ptr<KAutoObjectWithListContainer> global_object_list_container;

    std::unordered_set<KAutoObject*> registered_objects;

    /*
     * Synchronizes access to the internal HLE kernel structures, it is acquired when a guest
     * application thread performs a syscall. It should be acquired by any host threads that read or
     * modify the HLE kernel state. Note: Any operation that directly or indirectly reads from or
     * writes to the emulated memory is not protected by this mutex, and should be avoided in any
     * threads other than the CPU thread.
     */
    std::recursive_mutex hle_lock;

    /*
     * Flags non system module main threads to wait a bit before running. On real hardware,
     * system modules have plenty of time to load before the game is loaded, but on citra they
     * start at the same time as the game. The artificial wait gives system modules some time
     * to load and setup themselves before the game starts.
     */
    bool main_thread_extended_sleep = false;

    friend class boost::serialization::access;
    template <class Archive>
    void serialize(Archive& ar, const unsigned int);
};

} // namespace Kernel

BOOST_CLASS_EXPORT_KEY(Kernel::New3dsHwCapabilities)
