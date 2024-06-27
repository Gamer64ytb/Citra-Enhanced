// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <boost/serialization/shared_ptr.hpp>
#include <boost/serialization/unordered_map.hpp>
#include <boost/serialization/vector.hpp>
#include "common/archives.h"
#include "core/hle/kernel/config_mem.h"
#include "core/hle/kernel/ipc_debugger/recorder.h"
#include "core/hle/kernel/k_address_arbiter.h"
#include "core/hle/kernel/k_auto_object_container.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_handle_table.h"
#include "core/hle/kernel/k_linked_list.h"
#include "core/hle/kernel/k_mutex.h"
#include "core/hle/kernel/k_object_name.h"
#include "core/hle/kernel/k_port.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_resource_limit.h"
#include "core/hle/kernel/k_semaphore.h"
#include "core/hle/kernel/k_session.h"
#include "core/hle/kernel/k_shared_memory.h"
#include "core/hle/kernel/k_slab_heap.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/k_timer.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/memory.h"
#include "core/hle/kernel/shared_page.h"

SERIALIZE_EXPORT_IMPL(Kernel::New3dsHwCapabilities)

namespace Kernel {

/// Initialize the kernel
KernelSystem::KernelSystem(Memory::MemorySystem& memory, Core::Timing& timing,
                           std::function<void()> prepare_reschedule_callback,
                           MemoryMode memory_mode, u32 num_cores,
                           const New3dsHwCapabilities& n3ds_hw_caps, u64 override_init_time)
    : memory(memory), timing(timing),
      prepare_reschedule_callback(std::move(prepare_reschedule_callback)), memory_mode(memory_mode),
      n3ds_hw_caps(n3ds_hw_caps) {
    global_object_list_container = std::make_unique<KAutoObjectWithListContainer>(*this);
    object_name_global_data = std::make_unique<KObjectNameGlobalData>(*this);
    slab_heap_container = std::make_unique<SlabHeapContainer>();
    std::generate(memory_regions.begin(), memory_regions.end(),
                  [] { return std::make_shared<MemoryRegionInfo>(); });
    MemoryInit(memory_mode, n3ds_hw_caps.memory_mode, override_init_time);

    resource_limits = std::make_unique<ResourceLimitList>(*this);
    for (u32 core_id = 0; core_id < num_cores; ++core_id) {
        thread_managers.push_back(std::make_unique<ThreadManager>(*this, core_id));
    }
    timer_manager = std::make_unique<TimerManager>(timing);
    ipc_recorder = std::make_unique<IPCDebugger::Recorder>();
    stored_processes.assign(num_cores, nullptr);

    next_thread_id = 1;
}

/// Shutdown the kernel
KernelSystem::~KernelSystem() {
    ResetThreadIDs();
};

ResourceLimitList& KernelSystem::ResourceLimit() {
    return *resource_limits;
}

const ResourceLimitList& KernelSystem::ResourceLimit() const {
    return *resource_limits;
}

u32 KernelSystem::GenerateObjectID() {
    return next_object_id++;
}

Process* KernelSystem::GetCurrentProcess() const {
    return current_process;
}

void KernelSystem::SetCurrentProcess(Process* process) {
    current_process = process;
    SetCurrentMemoryPageTable(process->vm_manager.page_table);
}

void KernelSystem::SetCurrentProcessForCPU(Process* process, u32 core_id) {
    if (current_cpu->GetID() == core_id) {
        current_process = process;
        SetCurrentMemoryPageTable(process->vm_manager.page_table);
    } else {
        stored_processes[core_id] = process;
        thread_managers[core_id]->cpu->SetPageTable(process->vm_manager.page_table);
    }
}

void KernelSystem::SetCurrentMemoryPageTable(std::shared_ptr<Memory::PageTable> page_table) {
    memory.SetCurrentPageTable(page_table);
    if (current_cpu != nullptr) {
        current_cpu->SetPageTable(page_table);
    }
}

void KernelSystem::SetCPUs(std::vector<std::shared_ptr<Core::ARM_Interface>> cpus) {
    ASSERT(cpus.size() == thread_managers.size());
    for (u32 i = 0; i < cpus.size(); i++) {
        thread_managers[i]->SetCPU(*cpus[i]);
    }
}

void KernelSystem::SetRunningCPU(Core::ARM_Interface* cpu) {
    if (current_process) {
        stored_processes[current_cpu->GetID()] = current_process;
    }
    current_cpu = cpu;
    timing.SetCurrentTimer(cpu->GetID());
    if (stored_processes[current_cpu->GetID()]) {
        SetCurrentProcess(stored_processes[current_cpu->GetID()]);
    }
}

ThreadManager& KernelSystem::GetThreadManager(u32 core_id) {
    return *thread_managers[core_id];
}

const ThreadManager& KernelSystem::GetThreadManager(u32 core_id) const {
    return *thread_managers[core_id];
}

ThreadManager& KernelSystem::GetCurrentThreadManager() {
    return *thread_managers[current_cpu->GetID()];
}

const ThreadManager& KernelSystem::GetCurrentThreadManager() const {
    return *thread_managers[current_cpu->GetID()];
}

TimerManager& KernelSystem::GetTimerManager() {
    return *timer_manager;
}

const TimerManager& KernelSystem::GetTimerManager() const {
    return *timer_manager;
}

SharedPage::Handler& KernelSystem::GetSharedPageHandler() {
    return *shared_page_handler;
}

const SharedPage::Handler& KernelSystem::GetSharedPageHandler() const {
    return *shared_page_handler;
}

ConfigMem::Handler& KernelSystem::GetConfigMemHandler() {
    return *config_mem_handler;
}

IPCDebugger::Recorder& KernelSystem::GetIPCRecorder() {
    return *ipc_recorder;
}

const IPCDebugger::Recorder& KernelSystem::GetIPCRecorder() const {
    return *ipc_recorder;
}

u32 KernelSystem::NewThreadId() {
    return next_thread_id++;
}

u32 KernelSystem::NewProcessId() {
    return ++next_process_id;
}

void KernelSystem::ResetThreadIDs() {
    next_thread_id = 0;
}

template <class Archive>
void KernelSystem::serialize(Archive& ar, const unsigned int) {
    ar& memory_regions;
    // current_cpu set externally
    // NB: subsystem references and prepare_reschedule_callback are constant
    ar&* resource_limits.get();
    // ar& next_object_id;
    ar&* timer_manager.get();
    ar& next_process_id;
    ar& process_list;
    ar& current_process;
    // NB: core count checked in 'core'
    for (auto& thread_manager : thread_managers) {
        ar&* thread_manager.get();
    }
    ar& config_mem_handler;
    ar& shared_page_handler;
    ar& stored_processes;
    ar& next_thread_id;
    ar& memory_mode;
    ar& n3ds_hw_caps;
    ar& main_thread_extended_sleep;
    // Deliberately don't include debugger info to allow debugging through loads

    if (Archive::is_loading::value) {
        for (auto& memory_region : memory_regions) {
            memory_region->Unlock();
        }
        for (auto& process : process_list) {
            process->vm_manager.Unlock();
        }
    }
}

void KernelSystem::RegisterKernelObject(KAutoObject* object) {
    registered_objects.insert(object);
}

void KernelSystem::UnregisterKernelObject(KAutoObject* object) {
    registered_objects.erase(object);
}

// Constexpr counts.
constexpr size_t SlabHeapTotalSize = 0x450000;
constexpr size_t SlabCountKProcess = 47;
constexpr size_t SlabCountKThread = 300;
constexpr size_t SlabCountKEvent = 315;
constexpr size_t SlabCountKMutex = 85;
constexpr size_t SlabCountKSemaphore = 83;
constexpr size_t SlabCountKTimer = 60;
constexpr size_t SlabCountKPort = 153;
constexpr size_t SlabCountKSharedMemory = 63;
constexpr size_t SlabCountKSession = 345;
constexpr size_t SlabCountKAddressArbiter = 51;
constexpr size_t SlabCountKObjectName = 7;
constexpr size_t SlabCountKResourceLimit = 5;
// constexpr size_t SlabCountKDebug = 3;
constexpr size_t SlabCountKLinkedListNode = 4273;
// constexpr size_t SlabCountKBlockInfo = 601;
// constexpr size_t SlabCountKMemoryBlock = 1723;

struct KernelSystem::SlabHeapContainer {
    SlabHeapContainer() {
        // TODO: Allocate slab heap on FCRAM
        storage.resize(SlabHeapTotalSize);
        u8* memory = storage.data();
        event.Initialize(memory, SlabCountKEvent * sizeof(KEvent));
        memory += SlabCountKEvent * sizeof(KEvent);
        mutex.Initialize(memory, SlabCountKMutex * sizeof(KMutex));
        memory += SlabCountKMutex * sizeof(KMutex);
        semaphore.Initialize(memory, SlabCountKSemaphore * sizeof(KSemaphore));
        memory += SlabCountKSemaphore * sizeof(KSemaphore);
        timer.Initialize(memory, SlabCountKTimer * sizeof(KTimer));
        memory += SlabCountKTimer * sizeof(KTimer);
        process.Initialize(memory, SlabCountKProcess * sizeof(Process));
        memory += SlabCountKProcess * sizeof(Process);
        thread.Initialize(memory, SlabCountKThread * sizeof(KThread));
        memory += SlabCountKThread * sizeof(KThread);
        port.Initialize(memory, SlabCountKPort * sizeof(KPort));
        memory += SlabCountKPort * sizeof(KPort);
        shared_memory.Initialize(memory, SlabCountKSharedMemory * sizeof(KSharedMemory));
        memory += SlabCountKSharedMemory * sizeof(KSharedMemory);
        session.Initialize(memory, SlabCountKSession * sizeof(KSession));
        memory += SlabCountKSession * sizeof(KSession);
        resource_limit.Initialize(memory, SlabCountKResourceLimit * sizeof(KResourceLimit));
        memory += SlabCountKResourceLimit * sizeof(KResourceLimit);
        address_arbiter.Initialize(memory, SlabCountKAddressArbiter * sizeof(KAddressArbiter));
        memory += SlabCountKAddressArbiter * sizeof(KAddressArbiter);
        linked_list_node.Initialize(memory, SlabCountKLinkedListNode * sizeof(KLinkedListNode));
        memory += SlabCountKLinkedListNode * sizeof(KLinkedListNode);
        object_name.Initialize(memory, SlabCountKObjectName * sizeof(KObjectName));
    }

    std::vector<u8> storage;
    KSlabHeap<KEvent> event;
    KSlabHeap<KMutex> mutex;
    KSlabHeap<KSemaphore> semaphore;
    KSlabHeap<KTimer> timer;
    KSlabHeap<Process> process;
    KSlabHeap<KThread> thread;
    KSlabHeap<KPort> port;
    KSlabHeap<KSharedMemory> shared_memory;
    KSlabHeap<KSession> session;
    KSlabHeap<KResourceLimit> resource_limit;
    KSlabHeap<KAddressArbiter> address_arbiter;
    KSlabHeap<KLinkedListNode> linked_list_node;
    KSlabHeap<KObjectName> object_name;
};

template <typename T>
KSlabHeap<T>& KernelSystem::SlabHeap() {
    if constexpr (std::is_same_v<T, KEvent>) {
        return slab_heap_container->event;
    } else if constexpr (std::is_same_v<T, KPort>) {
        return slab_heap_container->port;
    } else if constexpr (std::is_same_v<T, Process>) {
        return slab_heap_container->process;
    } else if constexpr (std::is_same_v<T, KResourceLimit>) {
        return slab_heap_container->resource_limit;
    } else if constexpr (std::is_same_v<T, KSession>) {
        return slab_heap_container->session;
    } else if constexpr (std::is_same_v<T, KSharedMemory>) {
        return slab_heap_container->shared_memory;
    } else if constexpr (std::is_same_v<T, KThread>) {
        return slab_heap_container->thread;
    } else if constexpr (std::is_same_v<T, KAddressArbiter>) {
        return slab_heap_container->address_arbiter;
    } else if constexpr (std::is_same_v<T, KSemaphore>) {
        return slab_heap_container->semaphore;
    } else if constexpr (std::is_same_v<T, KMutex>) {
        return slab_heap_container->mutex;
    } else if constexpr (std::is_same_v<T, KObjectName>) {
        return slab_heap_container->object_name;
    } else if constexpr (std::is_same_v<T, KLinkedListNode>) {
        return slab_heap_container->linked_list_node;
    } else if constexpr (std::is_same_v<T, KTimer>) {
        return slab_heap_container->timer;
    }
    UNREACHABLE();
}

KAutoObjectWithListContainer& KernelSystem::ObjectListContainer() {
    return *global_object_list_container;
}

KObjectNameGlobalData& KernelSystem::ObjectNameGlobalData() {
    return *object_name_global_data;
}

template KSlabHeap<KEvent>& KernelSystem::SlabHeap();
template KSlabHeap<KPort>& KernelSystem::SlabHeap();
template KSlabHeap<Process>& KernelSystem::SlabHeap();
template KSlabHeap<KResourceLimit>& KernelSystem::SlabHeap();
template KSlabHeap<KSession>& KernelSystem::SlabHeap();
template KSlabHeap<KSharedMemory>& KernelSystem::SlabHeap();
template KSlabHeap<KThread>& KernelSystem::SlabHeap();
template KSlabHeap<KObjectName>& KernelSystem::SlabHeap();
template KSlabHeap<KAddressArbiter>& KernelSystem::SlabHeap();
template KSlabHeap<KSemaphore>& KernelSystem::SlabHeap();
template KSlabHeap<KMutex>& KernelSystem::SlabHeap();
template KSlabHeap<KLinkedListNode>& KernelSystem::SlabHeap();
template KSlabHeap<KTimer>& KernelSystem::SlabHeap();

SERIALIZE_IMPL(KernelSystem)

template <class Archive>
void New3dsHwCapabilities::serialize(Archive& ar, const unsigned int) {
    ar& enable_l2_cache;
    ar& enable_804MHz_cpu;
    ar& memory_mode;
}
SERIALIZE_IMPL(New3dsHwCapabilities)

} // namespace Kernel
