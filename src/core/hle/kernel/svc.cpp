// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <chrono>
#include <fmt/format.h>
#include "common/alignment.h"
#include "common/archives.h"
#include "common/logging/log.h"
#include "common/profiling.h"
#include "common/scm_rev.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/arm/arm_interface.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/gdbstub/hio.h"
#include "core/hle/kernel/config_mem.h"
#include "core/hle/kernel/errors.h"
#include "core/hle/kernel/ipc.h"
#include "core/hle/kernel/ipc_debugger/recorder.h"
#include "core/hle/kernel/k_address_arbiter.h"
#include "core/hle/kernel/k_client_port.h"
#include "core/hle/kernel/k_client_session.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_handle_table.h"
#include "core/hle/kernel/k_mutex.h"
#include "core/hle/kernel/k_object_name.h"
#include "core/hle/kernel/k_port.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_resource_limit.h"
#include "core/hle/kernel/k_scoped_resource_reservation.h"
#include "core/hle/kernel/k_semaphore.h"
#include "core/hle/kernel/k_server_port.h"
#include "core/hle/kernel/k_server_session.h"
#include "core/hle/kernel/k_session.h"
#include "core/hle/kernel/k_shared_memory.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/k_timer.h"
#include "core/hle/kernel/memory.h"
#include "core/hle/kernel/svc.h"
#include "core/hle/kernel/svc_wrapper.h"
#include "core/hle/kernel/vm_manager.h"
#include "core/hle/service/plgldr/plgldr.h"

namespace Kernel {

enum ControlMemoryOperation {
    MEMOP_FREE = 1,
    MEMOP_RESERVE = 2, // This operation seems to be unsupported in the kernel
    MEMOP_COMMIT = 3,
    MEMOP_MAP = 4,
    MEMOP_UNMAP = 5,
    MEMOP_PROTECT = 6,
    MEMOP_OPERATION_MASK = 0xFF,

    MEMOP_REGION_APP = 0x100,
    MEMOP_REGION_SYSTEM = 0x200,
    MEMOP_REGION_BASE = 0x300,
    MEMOP_REGION_MASK = 0xF00,

    MEMOP_LINEAR = 0x10000,
};

struct MemoryInfo {
    u32 base_address;
    u32 size;
    u32 permission;
    u32 state;
};

/// Values accepted by svcKernelSetState, only the known values are listed
/// (the behaviour of other values are known, but their purpose is unclear and irrelevant).
enum class KernelState {
    /**
     * Reboots the console
     */
    KERNEL_STATE_REBOOT = 7,

    // Special Citra only states.
    /**
     * Sets the emulation speed percentage. A value of 0 means unthrottled.
     */
    KERNEL_STATE_CITRA_EMULATION_SPEED = 0x20000 ///
};

struct PageInfo {
    u32 flags;
};

// Values accepted by svcGetHandleInfo.
enum class HandleInfoType {
    /**
     * Returns the time in ticks the KProcess referenced by the handle was created.
     */
    KPROCESS_ELAPSED_TICKS = 0,

    /**
     * Get internal refcount for kernel object.
     */
    REFERENCE_COUNT = 1,

    STUBBED_1 = 2,
    STUBBED_2 = 0x32107,
};

/// Values accepted by svcGetSystemInfo's type parameter.
enum class SystemInfoType {
    /**
     * Reports total used memory for all regions or a specific one, according to the extra
     * parameter. See `SystemInfoMemUsageRegion`.
     */
    REGION_MEMORY_USAGE = 0,
    /**
     * Returns the memory usage for certain allocations done internally by the kernel.
     */
    KERNEL_ALLOCATED_PAGES = 2,
    /**
     * "This returns the total number of processes which were launched directly by the kernel.
     * For the ARM11 NATIVE_FIRM kernel, this is 5, for processes sm, fs, pm, loader, and pxi."
     */
    KERNEL_SPAWNED_PIDS = 26,
    /**
     * Check if the current system is a new 3DS. This parameter is not available on real systems,
     * but can be used by homebrew applications.
     */
    NEW_3DS_INFO = 0x10001,
    /**
     * Gets citra related information. This parameter is not available on real systems,
     * but can be used by homebrew applications to get some emulator info.
     */
    CITRA_INFORMATION = 0x20000,
};

enum class ProcessInfoType {
    /**
     * Returns the amount of private (code, data, regular heap) and shared memory used by the
     * process + total supervisor-mode stack size + page-rounded size of the external handle table.
     * This is the amount of physical memory the process is using, minus TLS, main thread stack and
     * linear memory.
     */
    PRIVATE_AND_SHARED_USED_MEMORY = 0,

    /**
     * Returns the amount of <related unused field> + total supervisor-mode stack size +
     * page-rounded size of the external handle table.
     */
    SUPERVISOR_AND_HANDLE_USED_MEMORY = 1,

    /**
     * Returns the amount of private (code, data, heap) memory used by the process + total
     * supervisor-mode stack size + page-rounded size of the external handle table.
     */
    PRIVATE_SHARED_SUPERVISOR_HANDLE_USED_MEMORY = 2,

    /**
     * Returns the amount of <related unused field> + total supervisor-mode stack size +
     * page-rounded size of the external handle table.
     */
    SUPERVISOR_AND_HANDLE_USED_MEMORY2 = 3,

    /**
     * Returns the amount of handles in use by the process.
     */
    USED_HANDLE_COUNT = 4,

    /**
     * Returns the highest count of handles that have been open at once by the process.
     */
    HIGHEST_HANDLE_COUNT = 5,

    /**
     * Returns *(u32*)(KProcess+0x234) which is always 0.
     */
    KPROCESS_0X234 = 6,

    /**
     * Returns the number of threads of the process.
     */
    THREAD_COUNT = 7,

    /**
     * Returns the maximum number of threads which can be opened by this process (always 0).
     */
    MAX_THREAD_AMOUNT = 8,

    /**
     * Originally this only returned 0xD8E007ED. Now with v11.3 this returns the memregion for the
     * process: out low u32 = KProcess "Kernel flags from the exheader kernel descriptors" & 0xF00
     * (memory region flag). High out u32 = 0.
     */
    MEMORY_REGION_FLAGS = 19,

    /**
     * Low u32 = (0x20000000 - <LINEAR virtual-memory base for this process>). That is, the output
     * value is the value which can be added to LINEAR memory vaddrs for converting to
     * physical-memory addrs.
     */
    LINEAR_BASE_ADDR_OFFSET = 20,

    /**
     * Returns the VA -> PA conversion offset for the QTM static mem block reserved in the exheader
     * (0x800000), otherwise 0 (+ error 0xE0E01BF4) if it doesn't exist.
     */
    QTM_MEMORY_BLOCK_CONVERSION_OFFSET = 21,

    /**
     * Returns the base VA of the QTM static mem block reserved in the exheader, otherwise 0 (+
     * error 0xE0E01BF4) if it doesn't exist.
     */
    QTM_MEMORY_ADDRESS = 22,

    /**
     * Returns the size of the QTM static mem block reserved in the exheader, otherwise 0 (+ error
     * 0xE0E01BF4) if it doesn't exist.
     */
    QTM_MEMORY_SIZE = 23,

    // Custom values used by Luma3DS and 3GX plugins

    /**
     * Returns the process name.
     */
    LUMA_CUSTOM_PROCESS_NAME = 0x10000,

    /**
     * Returns the process title ID.
     */
    LUMA_CUSTOM_PROCESS_TITLE_ID = 0x10001,

    /**
     * Returns the codeset text size.
     */
    LUMA_CUSTOM_TEXT_SIZE = 0x10002,

    /**
     * Returns the codeset rodata size.
     */
    LUMA_CUSTOM_RODATA_SIZE = 0x10003,

    /**
     * Returns the codeset data size.
     */
    LUMA_CUSTOM_DATA_SIZE = 0x10004,

    /**
     * Returns the codeset text vaddr.
     */
    LUMA_CUSTOM_TEXT_ADDR = 0x10005,

    /**
     * Returns the codeset rodata vaddr.
     */
    LUMA_CUSTOM_RODATA_ADDR = 0x10006,

    /**
     * Returns the codeset data vaddr.
     */
    LUMA_CUSTOM_DATA_ADDR = 0x10007,

};

/**
 * Accepted by svcGetSystemInfo param with REGION_MEMORY_USAGE type. Selects a region to query
 * memory usage of.
 */
enum class SystemInfoMemUsageRegion {
    ALL = 0,
    APPLICATION = 1,
    SYSTEM = 2,
    BASE = 3,
};

/**
 * Accepted by svcGetSystemInfo param with CITRA_INFORMATION type. Selects which information
 * to fetch from Citra. Some string params don't fit in 7 bytes, so they are split.
 */
enum class SystemInfoCitraInformation {
    IS_CITRA = 0,          // Always set the output to 1, signaling the app is running on Citra.
    HOST_TICK = 1,         // Tick reference from the host in ns, unaffected by lag or cpu speed.
    EMULATION_SPEED = 2,   // Gets the emulation speed set by the user or by KernelSetState.
    BUILD_NAME = 10,       // (ie: Nightly, Canary).
    BUILD_VERSION = 11,    // Build version.
    BUILD_PLATFORM = 12,   // Build platform, see SystemInfoCitraPlatform.
    BUILD_DATE_PART1 = 20, // Build date first 7 characters.
    BUILD_DATE_PART2 = 21, // Build date next 7 characters.
    BUILD_DATE_PART3 = 22, // Build date next 7 characters.
    BUILD_DATE_PART4 = 23, // Build date last 7 characters.
    BUILD_GIT_BRANCH_PART1 = 30,      // Git branch first 7 characters.
    BUILD_GIT_BRANCH_PART2 = 31,      // Git branch last 7 characters.
    BUILD_GIT_DESCRIPTION_PART1 = 40, // Git description (commit) first 7 characters.
    BUILD_GIT_DESCRIPTION_PART2 = 41, // Git description (commit) last 7 characters.
};

/**
 * Current officially supported platforms.
 */
enum class SystemInfoCitraPlatform {
    PLATFORM_UNKNOWN = 0,
    PLATFORM_WINDOWS = 1,
    PLATFORM_LINUX = 2,
    PLATFORM_APPLE = 3,
    PLATFORM_ANDROID = 4,
};

/**
 * Accepted by the custom svcControlProcess.
 */
enum class ControlProcessOP {
    /**
     * List all handles of the process, varg3 can be either 0 to fetch
     * all handles, or token of the type to fetch s32 count =
     * svcControlProcess(handle, PROCESSOP_GET_ALL_HANDLES,
     * (u32)&outBuf, 0) Returns how many handles were found
     */
    PROCESSOP_GET_ALL_HANDLES = 0,

    /**
     * Set the whole memory of the process with rwx access (in the mmu
     * table only) svcControlProcess(handle, PROCESSOP_SET_MMU_TO_RWX,
     * 0, 0)
     */
    PROCESSOP_SET_MMU_TO_RWX,

    /**
     * Get the handle of an event which will be signaled
     * each time the memory layout of this process changes
     * svcControlProcess(handle,
     * PROCESSOP_GET_ON_MEMORY_CHANGE_EVENT,
     * &eventHandleOut, 0)
     */
    PROCESSOP_GET_ON_MEMORY_CHANGE_EVENT,

    /**
     * Set a flag to be signaled when the process will be exited
     * svcControlProcess(handle, PROCESSOP_SIGNAL_ON_EXIT, 0, 0)
     */
    PROCESSOP_SIGNAL_ON_EXIT,

    /**
     * Get the physical address of the VAddr within the process
     * svcControlProcess(handle, PROCESSOP_GET_PA_FROM_VA, (u32)&PAOut,
     * VAddr)
     */
    PROCESSOP_GET_PA_FROM_VA,

    /*
     * Lock / Unlock the process's threads
     * svcControlProcess(handle, PROCESSOP_SCHEDULE_THREADS, lock,
     * threadPredicate) lock: 0 to unlock threads, any other value to
     * lock threads threadPredicate: can be NULL or a funcptr to a
     * predicate (typedef bool (*ThreadPredicate)(KThread *thread);)
     * The predicate must return true to operate on the thread
     */
    PROCESSOP_SCHEDULE_THREADS,

    /*
     * Lock / Unlock the process's threads
     * svcControlProcess(handle, PROCESSOP_SCHEDULE_THREADS, lock,
     * tlsmagicexclude) lock: 0 to unlock threads, any other value to
     * lock threads tlsmagicexclude: do not lock threads with this tls magic
     * value
     */
    PROCESSOP_SCHEDULE_THREADS_WITHOUT_TLS_MAGIC,

    /**
     * Disable any thread creation restrictions, such as priority value
     * or allowed cores
     */
    PROCESSOP_DISABLE_CREATE_THREAD_RESTRICTIONS,
};

enum class BreakReason : u8 {
    Panic = 0,
    Assert = 1,
    User = 2,
};

class SVC : public SVCWrapper<SVC> {
public:
    SVC(Core::System& system);
    void CallSVC(u32 immediate);

private:
    Core::System& system;
    Kernel::KernelSystem& kernel;
    Memory::MemorySystem& memory;

    friend class SVCWrapper<SVC>;

    // ARM interfaces

    u32 GetReg(std::size_t n);
    void SetReg(std::size_t n, u32 value);

    // SVC interfaces

    Result ControlMemory(u32* out_addr, u32 addr0, u32 addr1, u32 size, u32 operation,
                         u32 permissions);
    void ExitProcess();
    Result TerminateProcess(Handle handle);
    Result MapMemoryBlock(Handle handle, u32 addr, u32 permissions, u32 other_permissions);
    Result UnmapMemoryBlock(Handle handle, u32 addr);
    Result ConnectToPort(Handle* out_handle, VAddr port_name_address);
    Result SendSyncRequest(Handle handle);
    Result OpenProcess(Handle* out_handle, u32 process_id);
    Result OpenThread(Handle* out_handle, Handle process_handle, u32 thread_id);
    Result CloseHandle(Handle handle);
    Result WaitSynchronization1(Handle handle, s64 nano_seconds);
    Result WaitSynchronizationN(s32* out, VAddr handles_address, s32 handle_count, bool wait_all,
                                s64 nano_seconds);
    Result ReplyAndReceive(s32* index, VAddr handles_address, s32 handle_count,
                           Handle reply_target);
    Result InvalidateProcessDataCache(Handle process_handle, VAddr address, u32 size);
    Result StoreProcessDataCache(Handle process_handle, VAddr address, u32 size);
    Result FlushProcessDataCache(Handle process_handle, VAddr address, u32 size);
    Result CreateAddressArbiter(Handle* out_handle);
    Result ArbitrateAddress(Handle handle, u32 address, u32 type, u32 value, s64 nanoseconds);
    void Break(u8 break_reason);
    void OutputDebugString(VAddr address, s32 len);
    Result GetResourceLimit(Handle* resource_limit, Handle process_handle);
    Result GetResourceLimitCurrentValues(VAddr values, Handle resource_limit_handle, VAddr names,
                                         u32 name_count);
    Result GetResourceLimitLimitValues(VAddr values, Handle resource_limit_handle, VAddr names,
                                       u32 name_count);
    Result SetResourceLimitLimitValues(Handle res_limit, VAddr names, VAddr resource_list,
                                       u32 name_count);
    Result CreateThread(Handle* out_handle, u32 entry_point, u32 arg, VAddr stack_top, u32 priority,
                        s32 processor_id);
    void ExitThread();
    Result GetThreadPriority(u32* priority, Handle handle);
    Result SetThreadPriority(Handle handle, u32 priority);
    Result CreateMutex(Handle* out_handle, u32 initial_locked);
    Result ReleaseMutex(Handle handle);
    Result GetProcessId(u32* process_id, Handle process_handle);
    Result GetProcessIdOfThread(u32* process_id, Handle thread_handle);
    Result GetThreadId(u32* thread_id, Handle handle);
    Result CreateSemaphore(Handle* out_handle, s32 initial_count, s32 max_count);
    Result ReleaseSemaphore(s32* count, Handle handle, s32 release_count);
    Result KernelSetState(u32 kernel_state, u32 varg1, u32 varg2);
    Result QueryProcessMemory(MemoryInfo* memory_info, PageInfo* page_info, Handle process_handle,
                              u32 addr);
    Result QueryMemory(MemoryInfo* memory_info, PageInfo* page_info, u32 addr);
    Result CreateEvent(Handle* out_handle, u32 reset_type);
    Result DuplicateHandle(Handle* out, Handle handle);
    Result SignalEvent(Handle handle);
    Result ClearEvent(Handle handle);
    Result CreateTimer(Handle* out_handle, u32 reset_type);
    Result ClearTimer(Handle handle);
    Result SetTimer(Handle handle, s64 initial, s64 interval);
    Result CancelTimer(Handle handle);
    void SleepThread(s64 nanoseconds);
    s64 GetSystemTick();
    Result GetHandleInfo(s64* out, Handle handle, u32 type);
    Result CreateMemoryBlock(Handle* out_handle, u32 addr, u32 size, u32 my_permission,
                             u32 other_permission);
    Result CreatePort(Handle* server_port, Handle* client_port, VAddr name_address,
                      u32 max_sessions);
    Result CreateSessionToPort(Handle* out_client_session, Handle client_port_handle);
    Result CreateSession(Handle* server_session, Handle* client_session);
    Result AcceptSession(Handle* out_server_session, Handle server_port_handle);
    Result GetSystemInfo(s64* out, u32 type, s32 param);
    Result GetProcessInfo(s64* out, Handle process_handle, u32 type);
    Result GetThreadInfo(s64* out, Handle thread_handle, u32 type);
    Result GetProcessList(s32* process_count, VAddr out_process_array, s32 out_process_array_count);
    Result InvalidateInstructionCacheRange(u32 addr, u32 size);
    Result InvalidateEntireInstructionCache();
    u32 ConvertVaToPa(u32 addr);
    Result MapProcessMemoryEx(Handle dst_process_handle, u32 dst_address, Handle src_process_handle,
                              u32 src_address, u32 size);
    Result UnmapProcessMemoryEx(Handle process, u32 dst_address, u32 size);
    Result ControlProcess(Handle process_handle, u32 process_OP, u32 varg2, u32 varg3);

    struct FunctionDef {
        using Func = void (SVC::*)();

        u32 id;
        Func func;
        const char* name;
    };

    static const std::array<FunctionDef, 180> SVC_Table;
    static const FunctionDef* GetSVCInfo(u32 func_num);
};

/// Map application or GSP heap memory
Result SVC::ControlMemory(u32* out_addr, u32 addr0, u32 addr1, u32 size, u32 operation,
                          u32 permissions) {
    LOG_DEBUG(Kernel_SVC,
              "called operation=0x{:08X}, addr0=0x{:08X}, addr1=0x{:08X}, "
              "size=0x{:X}, permissions=0x{:08X}",
              operation, addr0, addr1, size, permissions);

    R_UNLESS((addr0 & Memory::CITRA_PAGE_MASK) == 0, ResultMisalignedAddress);
    R_UNLESS((addr1 & Memory::CITRA_PAGE_MASK) == 0, ResultMisalignedAddress);
    R_UNLESS((size & Memory::CITRA_PAGE_MASK) == 0, ResultMisalignedSize);

    const u32 region = operation & MEMOP_REGION_MASK;
    operation &= ~MEMOP_REGION_MASK;

    if (region != 0) {
        LOG_WARNING(Kernel_SVC, "ControlMemory with specified region not supported, region={:X}",
                    region);
    }

    if ((permissions & (u32)MemoryPermission::ReadWrite) != permissions) {
        return ResultInvalidCombination;
    }
    VMAPermission vma_permissions = (VMAPermission)permissions;

    auto& process = *kernel.GetCurrentProcess();

    switch (operation & MEMOP_OPERATION_MASK) {
    case MEMOP_FREE: {
        // TODO(Subv): What happens if an application tries to FREE a block of memory that has a
        // SharedMemory pointing to it?
        if (addr0 >= Memory::HEAP_VADDR && addr0 < Memory::HEAP_VADDR_END) {
            R_TRY(process.HeapFree(addr0, size));
        } else if (addr0 >= process.GetLinearHeapBase() && addr0 < process.GetLinearHeapLimit()) {
            R_TRY(process.LinearFree(addr0, size));
        } else {
            return ResultInvalidAddress;
        }
        *out_addr = addr0;
        break;
    }
    case MEMOP_COMMIT: {
        if (operation & MEMOP_LINEAR) {
            return process.LinearAllocate(out_addr, addr0, size, vma_permissions);
        } else {
            return process.HeapAllocate(out_addr, addr0, size, vma_permissions);
        }
        break;
    }
    case MEMOP_MAP: {
        return process.Map(addr0, addr1, size, vma_permissions);
    }
    case MEMOP_UNMAP: {
        return process.Unmap(addr0, addr1, size, vma_permissions);
    }
    case MEMOP_PROTECT: {
        return process.vm_manager.ReprotectRange(addr0, size, vma_permissions);
    }
    default:
        LOG_ERROR(Kernel_SVC, "unknown operation=0x{:08X}", operation);
        return ResultInvalidCombination;
    }

    return ResultSuccess;
}

void SVC::ExitProcess() {
    kernel.TerminateProcess(kernel.GetCurrentProcess());
}

Result SVC::TerminateProcess(Handle handle) {
    // Get the current process and handle table.
    auto current_process = kernel.GetCurrentProcess();
    auto& handle_table = current_process->handle_table;

    // Get the process to terminate.
    KScopedAutoObject process = handle_table.GetObject<Process>(handle);
    R_UNLESS(process.IsNotNull(), ResultInvalidHandle);

    // Terminate it.
    kernel.TerminateProcess(process.GetPointerUnsafe());
    return ResultSuccess;
}

Result SVC::MapMemoryBlock(Handle handle, u32 addr, u32 permissions, u32 other_permissions) {
    // Get the current process and handle table.
    auto current_process = kernel.GetCurrentProcess();
    auto& handle_table = current_process->handle_table;

    // Validate the address alignment.
    R_UNLESS(Common::Is4KBAligned(addr), ResultMisalignedAddress);

    // Get the shared memory.
    KScopedAutoObject shared_memory = handle_table.GetObject<KSharedMemory>(handle);
    R_UNLESS(shared_memory.IsNotNull(), ResultInvalidHandle);

    // Map shared memory.
    MemoryPermission permissions_type = static_cast<MemoryPermission>(permissions);
    switch (permissions_type) {
    case MemoryPermission::Read:
    case MemoryPermission::Write:
    case MemoryPermission::ReadWrite:
    case MemoryPermission::Execute:
    case MemoryPermission::ReadExecute:
    case MemoryPermission::WriteExecute:
    case MemoryPermission::ReadWriteExecute:
    case MemoryPermission::DontCare:
        return shared_memory->Map(*current_process, addr, permissions_type,
                                  static_cast<MemoryPermission>(other_permissions));
    default:
        LOG_ERROR(Kernel_SVC, "unknown permissions=0x{:08X}", permissions);
    }

    return ResultInvalidCombination;
}

Result SVC::UnmapMemoryBlock(Handle handle, u32 addr) {
    // Get the current process and handle table.
    auto current_process = kernel.GetCurrentProcess();
    auto& handle_table = current_process->handle_table;

    // Validate the address alignment.
    R_UNLESS(Common::Is4KBAligned(addr), ResultMisalignedAddress);

    // TODO(Subv): Return E0A01BF5 if the address is not in the application's heap

    // Get the shared memory.
    KScopedAutoObject shared_memory = handle_table.GetObject<KSharedMemory>(handle);
    R_UNLESS(shared_memory.IsNotNull(), ResultInvalidHandle);

    // Unmap the shared memory.
    return shared_memory->Unmap(*current_process, addr);
}

Result SVC::ConnectToPort(Handle* out_handle, VAddr port_name_address) {
    // Get the current process and handle table.
    auto current_process = kernel.GetCurrentProcess();
    auto& handle_table = current_process->handle_table;

    // Ensure the port virtual address is valid.
    R_UNLESS(memory.IsValidVirtualAddress(*current_process, port_name_address), ResultNotFound);

    // Read 1 char beyond the max allowed port name to detect names that are too long.
    static constexpr std::size_t PortNameMaxLength = 11;
    const auto port_name = memory.ReadCString(port_name_address, PortNameMaxLength + 1);
    R_UNLESS(port_name.size() <= PortNameMaxLength, ResultPortNameTooLong);

    // Find the client port.
    auto port = KObjectName::Find<KClientPort>(system.Kernel(), port_name.data());
    R_UNLESS(port.IsNotNull(), ResultNotFound);

    // Create a session.
    KClientSession* session{};
    R_TRY(port->CreateSession(std::addressof(session)));

    // Add the session in the table, close the extra reference.
    handle_table.Add(out_handle, session);
    session->Close();

    // We succeeded.
    return ResultSuccess;
}

Result SVC::SendSyncRequest(Handle handle) {
    // Get the current process and handle table.
    auto current_process = kernel.GetCurrentProcess();
    auto& handle_table = current_process->handle_table;

    // Get the client session.
    KScopedAutoObject session = handle_table.GetObject<KClientSession>(handle);
    R_UNLESS(session.IsNotNull(), ResultInvalidHandle);

    // Reschedule.
    system.PrepareReschedule();

    // Record the sync request in the IPC recorder.
    auto thread = kernel.GetCurrentThreadManager().GetCurrentThread();
    auto& ipc_recorder = kernel.GetIPCRecorder();
    if (ipc_recorder.IsEnabled()) {
        ipc_recorder.RegisterRequest(session.GetPointerUnsafe(), thread);
    }

    // Perform the sync request.
    [[maybe_unused]] const u32 pc = system.GetRunningCore().GetReg(15);
    [[maybe_unused]] const u32 lr = system.GetRunningCore().GetReg(14);
    return session->SendSyncRequest(thread);
}

Result SVC::OpenProcess(Handle* out_handle, u32 process_id) {
    // Find the process with the provided pid.
    Process* process = kernel.GetProcessById(process_id);
    R_UNLESS(process, ResultProcessNotFound);

    // Open a reference to the process.
    process->Open();
    SCOPE_EXIT({ process->Close(); });

    // Add the process to the handle table.
    auto& handle_table = kernel.GetCurrentProcess()->handle_table;
    return handle_table.Add(out_handle, process);
}

Result SVC::OpenThread(Handle* out_handle, Handle handle, u32 thread_id) {
    static constexpr Result ResultThreadNotFound =
        Result(25, ErrorModule::OS, ErrorSummary::WrongArgument, ErrorLevel::Permanent);

    // TODO: Implement OpenThread with process_handle = 0
    R_UNLESS(handle != 0, ResultThreadNotFound);

    // Get the current process and handle table.
    auto current_process = kernel.GetCurrentProcess();
    auto& handle_table = current_process->handle_table;

    // Get the process.
    KScopedAutoObject process = handle_table.GetObject<Process>(handle);
    R_UNLESS(process.IsNotNull(), ResultInvalidHandle);

    for (u32 core_id = 0; core_id < system.GetNumCores(); core_id++) {
        const auto thread_list = kernel.GetThreadManager(core_id).GetThreadList();
        for (auto& thread : thread_list) {
            if (thread->GetOwner() != process.GetPointerUnsafe() ||
                thread->GetThreadId() != thread_id) {
                continue;
            }

            // Add the found thread to the provided processes handle table.
            return handle_table.Add(out_handle, thread);
        }
    }

    // Did not find any thread
    return ResultThreadNotFound;
}

Result SVC::CloseHandle(Handle handle) {
    // Get the current process and handle table.
    auto current_process = kernel.GetCurrentProcess();
    auto& handle_table = current_process->handle_table;

    // Close the handle
    R_UNLESS(handle_table.Remove(handle), ResultInvalidHandle);
    return ResultSuccess;
}

static Result ReceiveIPCRequest(Kernel::KernelSystem& kernel, Memory::MemorySystem& memory,
                                KServerSession* server_session, KThread* thread);

class SVC_SyncCallback : public Kernel::WakeupCallback {
public:
    explicit SVC_SyncCallback(bool do_output_) : do_output(do_output_) {}
    void WakeUp(ThreadWakeupReason reason, KThread* thread, KSynchronizationObject* object) {

        if (reason == ThreadWakeupReason::Timeout) {
            thread->SetWaitSynchronizationResult(ResultTimeout);
            return;
        }

        ASSERT(reason == ThreadWakeupReason::Signal);
        thread->SetWaitSynchronizationResult(ResultSuccess);

        // The wait_all case does not update the output index.
        if (do_output) {
            thread->SetWaitSynchronizationOutput(thread->GetWaitObjectIndex(object));
        }
    }

private:
    bool do_output;

    SVC_SyncCallback() = default;
    template <class Archive>
    void serialize(Archive& ar, const unsigned int) {
        ar& boost::serialization::base_object<Kernel::WakeupCallback>(*this);
        ar& do_output;
    }
    friend class boost::serialization::access;
};

class SVC_IPCCallback : public Kernel::WakeupCallback {
public:
    explicit SVC_IPCCallback(Core::System& system_) : system(system_) {}

    void WakeUp(ThreadWakeupReason reason, KThread* thread, KSynchronizationObject* object) {

        ASSERT(thread->GetStatus() == ThreadStatus::WaitSynchAny);
        ASSERT(reason == ThreadWakeupReason::Signal);

        Result result = ResultSuccess;
        if (object->GetTypeObj().GetClassToken() == ClassTokenType::KServerSession) {
            auto server_session = object->DynamicCast<KServerSession*>();
            result = ReceiveIPCRequest(system.Kernel(), system.Memory(), server_session, thread);
        }

        thread->SetWaitSynchronizationResult(result);
        thread->SetWaitSynchronizationOutput(thread->GetWaitObjectIndex(object));
    }

private:
    Core::System& system;

    SVC_IPCCallback() : system(Core::Global<Core::System>()) {}

    template <class Archive>
    void serialize(Archive& ar, const unsigned int) {
        ar& boost::serialization::base_object<Kernel::WakeupCallback>(*this);
    }
    friend class boost::serialization::access;
};

/// Wait for a handle to synchronize, timeout after the specified nanoseconds
Result SVC::WaitSynchronization1(Handle handle, s64 nano_seconds) {
    KScopedAutoObject object =
        kernel.GetCurrentProcess()->handle_table.GetObject<KSynchronizationObject>(handle);
    R_UNLESS(object.IsNotNull(), ResultInvalidHandle);

    KThread* thread = kernel.GetCurrentThreadManager().GetCurrentThread();
    if (object->ShouldWait(thread)) {
        R_UNLESS(nano_seconds != 0, ResultTimeout);

        // Add the object to the threads wait list.
        thread->m_wait_objects = {object.GetPointerUnsafe()};
        object->AddWaitingThread(thread);
        thread->m_status = ThreadStatus::WaitSynchAny;

        // Create an event to wake the thread up after the specified nanosecond delay has passed
        thread->WakeAfterDelay(nano_seconds);
        thread->m_wakeup_callback = std::make_shared<SVC_SyncCallback>(false);

        system.PrepareReschedule();

        // Note: The output of this SVC will be set to ResultSuccess if the thread
        // resumes due to a signal in its wait objects.
        // Otherwise we retain the default value of timeout.
        return ResultTimeout;
    }

    object->Acquire(thread);
    return ResultSuccess;
}

/// Wait for the given handles to synchronize, timeout after the specified nanoseconds
Result SVC::WaitSynchronizationN(s32* out, VAddr handles_address, s32 num_handles, bool wait_all,
                                 s64 nano_seconds) {
    // Get handle table.
    auto& handle_table = kernel.GetCurrentProcess()->handle_table;
    KThread* thread = kernel.GetCurrentThreadManager().GetCurrentThread();

    // Ensure address and handle count are valid.
    R_UNLESS(memory.IsValidVirtualAddress(*kernel.GetCurrentProcess(), handles_address),
             ResultInvalidPointer);
    R_UNLESS(num_handles >= 0, ResultOutOfRange);

    std::vector<Handle> handles(num_handles);
    std::vector<KSynchronizationObject*> objects(num_handles);

    // Copy user handles.
    if (num_handles > 0) {
        // Get the handles.
        memory.ReadBlock(handles_address, handles.data(), sizeof(Handle) * num_handles);

        // Convert the handles to objects.
        R_UNLESS(handle_table.GetMultipleObjects<KSynchronizationObject>(
                     objects.data(), handles.data(), num_handles),
                 ResultInvalidHandle);
    }

    // Ensure handles are closed when we're done.
    SCOPE_EXIT({
        for (auto i = 0; i < num_handles; ++i) {
            objects[i]->Close();
        }
    });

    if (wait_all) {
        const bool all_available = std::ranges::all_of(
            objects, [thread](const auto object) { return !object->ShouldWait(thread); });
        if (all_available) {
            // We can acquire all objects right now, do so.
            for (auto& object : objects) {
                object->Acquire(thread);
            }
            // Note: In this case, the `out` parameter is not set,
            // and retains whatever value it had before.
            return ResultSuccess;
        }

        // Not all objects were available right now, prepare to suspend the thread.

        // If a timeout value of 0 was provided, just return the Timeout error code instead of
        // suspending the thread.
        R_UNLESS(nano_seconds != 0, ResultTimeout);

        // Put the thread to sleep
        thread->m_status = ThreadStatus::WaitSynchAll;

        // Add the thread to each of the objects' waiting threads.
        for (auto& object : objects) {
            object->AddWaitingThread(thread);
        }

        thread->m_wait_objects = std::move(objects);

        // Create an event to wake the thread up after the specified nanosecond delay has passed
        thread->WakeAfterDelay(nano_seconds);
        thread->m_wakeup_callback = std::make_shared<SVC_SyncCallback>(false);
        system.PrepareReschedule();

        // This value gets set to -1 by default in this case, it is not modified after this.
        *out = -1;
        // Note: The output of this SVC will be set to ResultSuccess if the thread resumes due to
        // a signal in one of its wait objects.
        return ResultTimeout;
    } else {
        // Find the first object that is acquirable in the provided list of objects
        auto itr = std::ranges::find_if(
            objects, [thread](const auto object) { return !object->ShouldWait(thread); });

        if (itr != objects.end()) {
            // We found a ready object, acquire it and set the result value
            KSynchronizationObject* object = *itr;
            object->Acquire(thread);
            *out = static_cast<s32>(std::distance(objects.begin(), itr));
            return ResultSuccess;
        }

        // No objects were ready to be acquired, prepare to suspend the thread.

        // If a timeout value of 0 was provided, just return the Timeout error code instead of
        // suspending the thread.
        R_UNLESS(nano_seconds != 0, ResultTimeout);

        // Put the thread to sleep
        thread->m_status = ThreadStatus::WaitSynchAny;

        // Add the thread to each of the objects' waiting threads.
        for (KSynchronizationObject* object : objects) {
            object->AddWaitingThread(thread);
        }

        thread->m_wait_objects = std::move(objects);

        // Note: If no handles and no timeout were given, then the thread will deadlock, this is
        // consistent with hardware behavior.

        // Create an event to wake the thread up after the specified nanosecond delay has passed
        thread->WakeAfterDelay(nano_seconds);
        thread->m_wakeup_callback = std::make_shared<SVC_SyncCallback>(true);
        system.PrepareReschedule();

        // Note: The output of this SVC will be set to ResultSuccess if the thread resumes due to a
        // signal in one of its wait objects.
        // Otherwise we retain the default value of timeout, and -1 in the out parameter
        *out = -1;
        return ResultTimeout;
    }
}

static Result ReceiveIPCRequest(Kernel::KernelSystem& kernel, Memory::MemorySystem& memory,
                                KServerSession* server_session, KThread* thread) {
    R_UNLESS(server_session, ResultSessionClosed);

    KThread* currently_handling = server_session->GetCurrent();
    VAddr target_address = thread->GetCommandBufferAddress();
    VAddr source_address = currently_handling->GetCommandBufferAddress();

    const Result translation_result =
        TranslateCommandBuffer(kernel, memory, currently_handling, thread, source_address,
                               target_address, server_session->GetMappedBufferContext(), false);

    // If a translation error occurred, immediately resume the client thread.
    if (translation_result.IsError()) {
        // Set the output of SendSyncRequest in the client thread to the translation output.
        currently_handling->SetWaitSynchronizationResult(translation_result);

        // Wake up the thread.
        currently_handling->ResumeFromWait();
        currently_handling = nullptr;

        // TODO(Subv): This path should try to wait again on the same objects.
        ASSERT_MSG(false, "ReplyAndReceive translation error behavior unimplemented");
    }

    return translation_result;
}

/// In a single operation, sends a IPC reply and waits for a new request.
Result SVC::ReplyAndReceive(s32* index, VAddr handles_address, s32 num_handles,
                            Handle reply_target) {
    // Retrieve current process and handle table.
    auto current_process = kernel.GetCurrentProcess();
    auto& handle_table = current_process->handle_table;

    // Ensure number of handles and handles array are valid.
    R_UNLESS(memory.IsValidVirtualAddress(*current_process, handles_address), ResultInvalidPointer);
    R_UNLESS(0 <= num_handles, ResultOutOfRange);

    // Retrieve the waitable objects from the handle table.
    std::vector<Handle> handles(num_handles);
    std::vector<KSynchronizationObject*> objects(num_handles);

    // Copy user handles.
    if (num_handles > 0) {
        // Get the handles.
        memory.ReadBlock(handles_address, handles.data(), sizeof(Handle) * num_handles);

        // Convert the handles to objects.
        R_UNLESS(handle_table.GetMultipleObjects<KSynchronizationObject>(
                     objects.data(), handles.data(), num_handles),
                 ResultInvalidHandle);
    }

    // Ensure handles are closed when we're done.
    SCOPE_EXIT({
        for (auto i = 0; i < num_handles; ++i) {
            objects[i]->Close();
        }
    });

    // We are also sending a command reply.
    // Do not send a reply if the command id in the command buffer is 0xFFFF.
    KThread* thread = kernel.GetCurrentThreadManager().GetCurrentThread();
    const u32 cmd_buff_header = memory.Read32(thread->GetCommandBufferAddress());
    const IPC::Header header{cmd_buff_header};
    if (reply_target != 0 && header.command_id != 0xFFFF) {
        // Get the session from its handle.
        KScopedAutoObject session = handle_table.GetObject<KServerSession>(reply_target);
        R_UNLESS(session.IsNotNull(), ResultInvalidHandle);

        // Mark the request as "handled".
        auto request_thread = std::exchange(session->currently_handling, nullptr);

        // Error out if there's no request thread or the session was closed.
        R_UNLESS(request_thread, ResultSessionClosed);
        R_UNLESS(session->GetParent()->GetState() == KSessionState::Normal, ResultSessionClosed);

        VAddr source_address = thread->GetCommandBufferAddress();
        VAddr target_address = request_thread->GetCommandBufferAddress();

        const Result translation_result =
            TranslateCommandBuffer(kernel, memory, thread, request_thread, source_address,
                                   target_address, session->GetMappedBufferContext(), true);

        // Note: The real kernel seems to always panic if the Server->Client buffer translation
        // fails for whatever reason.
        R_ASSERT(translation_result);

        // Note: The scheduler is not invoked here.
        request_thread->ResumeFromWait();
    }

    if (num_handles == 0) {
        *index = 0;
        R_SUCCEED_IF(reply_target != 0 && header.command_id != 0xFFFF);
        // The kernel uses this value as a placeholder for the real error, and returns it when we
        // pass no handles and do not perform any reply.
        if (reply_target == 0 || header.command_id == 0xFFFF) {
            return Result(0xE7E3FFFF);
        }

        return ResultSuccess;
    }

    // Find the first object that is acquirable in the provided list of objects
    auto it = std::find_if(objects.begin(), objects.end(),
                           [thread](const auto* object) { return !object->ShouldWait(thread); });

    if (it != objects.end()) {
        // We found a ready object, acquire it and set the result value
        KSynchronizationObject* object = *it;
        object->Acquire(thread);
        *index = static_cast<s32>(std::distance(objects.begin(), it));

        // If not a server session we are done.
        R_SUCCEED_IF(object->GetTypeObj().GetClassToken() != ClassTokenType::KServerSession);

        // Otherwise receive the IPC request.
        auto server_session = object->DynamicCast<KServerSession*>();
        return ReceiveIPCRequest(kernel, memory, server_session, thread);
    }

    // No objects were ready to be acquired, prepare to suspend the thread.

    // Put the thread to sleep
    thread->m_status = ThreadStatus::WaitSynchAny;

    // Add the thread to each of the objects' waiting threads.
    for (KSynchronizationObject* object : objects) {
        object->AddWaitingThread(thread);
    }

    // Set the wakeup callback.
    thread->m_wait_objects = std::move(objects);
    thread->m_wakeup_callback = std::make_shared<SVC_IPCCallback>(system);
    system.PrepareReschedule();

    // Note: The output of this SVC will be set to ResultSuccess if the thread resumes due to a
    // signal in one of its wait objects, or to 0xC8A01836 if there was a translation error.
    // By default the index is set to -1.
    *index = -1;
    return ResultSuccess;
}

/// Invalidates the specified cache range (stubbed as we do not emulate cache).
Result SVC::InvalidateProcessDataCache(Handle process_handle, VAddr address, u32 size) {
    // Get the process.
    KScopedAutoObject process =
        kernel.GetCurrentProcess()->handle_table.GetObject<Process>(process_handle);
    R_UNLESS(process.IsNotNull(), ResultInvalidHandle);

    LOG_DEBUG(Kernel_SVC, "called address=0x{:08X}, size=0x{:08X}", address, size);

    return ResultSuccess;
}

/// Stores the specified cache range (stubbed as we do not emulate cache).
Result SVC::StoreProcessDataCache(Handle process_handle, VAddr address, u32 size) {
    // Get the process.
    KScopedAutoObject process =
        kernel.GetCurrentProcess()->handle_table.GetObject<Process>(process_handle);
    R_UNLESS(process.IsNotNull(), ResultInvalidHandle);

    LOG_DEBUG(Kernel_SVC, "called address=0x{:08X}, size=0x{:08X}", address, size);

    return ResultSuccess;
}

/// Flushes the specified cache range (stubbed as we do not emulate cache).
Result SVC::FlushProcessDataCache(Handle process_handle, VAddr address, u32 size) {
    // Get the process.
    KScopedAutoObject process =
        kernel.GetCurrentProcess()->handle_table.GetObject<Process>(process_handle);
    R_UNLESS(process.IsNotNull(), ResultInvalidHandle);

    LOG_DEBUG(Kernel_SVC, "called address=0x{:08X}, size=0x{:08X}", address, size);

    return ResultSuccess;
}

/// Create an address arbiter (to allocate access to shared resources)
Result SVC::CreateAddressArbiter(Handle* out_handle) {
    static constexpr Result ResultOutOfArbiters =
        Result(ErrCodes::OutOfAddressArbiters, ErrorModule::OS, ErrorSummary::OutOfResource,
               ErrorLevel::Status);

    // Get the current process and handle table.
    auto process = kernel.GetCurrentProcess();
    auto& handle_table = process->handle_table;

    // Reserve a new address arbiter from the process resource limit.
    KScopedResourceReservation arb_reservation(process, ResourceLimitType::AddressArbiter);
    R_UNLESS(arb_reservation.Succeeded(), ResultOutOfArbiters);

    // Create the address arbiter.
    KAddressArbiter* arbiter = KAddressArbiter::Create(kernel);
    R_UNLESS(arbiter != nullptr, Result{0xC8601801});

    // Ensure the only reference is in the handle table when we're done.
    SCOPE_EXIT({ arbiter->Close(); });

    // Initialize the address arbiter.
    arbiter->Initialize(process);

    // Commit the reservation.
    arb_reservation.Commit();

    // Register the address arbiter.
    KAddressArbiter::Register(kernel, arbiter);

    // Add the address arbiter to the handle table.
    return handle_table.Add(out_handle, arbiter);
}

Result SVC::ArbitrateAddress(Handle handle, u32 address, u32 type, u32 value, s64 nanoseconds) {
    // Get the handle table.
    auto& handle_table = kernel.GetCurrentProcess()->handle_table;

    // Get the address arbiter.
    KScopedAutoObject arbiter = handle_table.GetObject<KAddressArbiter>(handle);
    R_UNLESS(arbiter.IsNotNull(), ResultInvalidHandle);

    // TODO(Subv): Identify in which specific cases this call should cause a reschedule.
    system.PrepareReschedule();

    // Arbitrate the address.
    KThread* thread = kernel.GetCurrentThreadManager().GetCurrentThread();
    return arbiter->ArbitrateAddress(thread, static_cast<ArbitrationType>(type), address, value,
                                     nanoseconds);
}

void SVC::Break(u8 break_reason) {
    LOG_CRITICAL(Debug_Emulated, "Emulated program broke execution!");

    const auto reason = static_cast<BreakReason>(break_reason);
    const auto reason_name = [&] {
        switch (reason) {
        case BreakReason::Panic:
            return "PANIC";
        case BreakReason::Assert:
            return "ASSERT";
        case BreakReason::User:
            return "USER";
        default:
            return "UNKNOWN";
        }
    }();

    LOG_CRITICAL(Debug_Emulated, "Break reason: {}", reason_name);
    system.SetStatus(Core::System::ResultStatus::ErrorUnknown);
}

void SVC::OutputDebugString(VAddr address, s32 len) {
    if (!memory.IsValidVirtualAddress(*kernel.GetCurrentProcess(), address)) {
        LOG_WARNING(Kernel_SVC, "OutputDebugString called with invalid address {:X}", address);
        return;
    }

    if (len == 0) {
        GDBStub::SetHioRequest(system, address);
        return;
    }

    if (len < 0) {
        LOG_WARNING(Kernel_SVC, "OutputDebugString called with invalid length {}", len);
        return;
    }

    std::string string(len, ' ');
    memory.ReadBlock(*kernel.GetCurrentProcess(), address, string.data(), len);
    LOG_DEBUG(Debug_Emulated, "{}", string);
}

Result SVC::GetResourceLimit(Handle* resource_limit, Handle process_handle) {
    // Get the handle table.
    auto& handle_table = kernel.GetCurrentProcess()->handle_table;

    // Get the process.
    KScopedAutoObject process = handle_table.GetObject<Process>(process_handle);
    R_UNLESS(process.IsNotNull(), ResultInvalidHandle);

    // Add the resource limit
    return handle_table.Add(resource_limit, process->resource_limit);
}

Result SVC::GetResourceLimitCurrentValues(VAddr values, Handle resource_limit_handle, VAddr names,
                                          u32 name_count) {
    // Get the handle table.
    auto& handle_table = kernel.GetCurrentProcess()->handle_table;

    // Get the resource limit.
    KScopedAutoObject resource_limit =
        handle_table.GetObject<KResourceLimit>(resource_limit_handle);
    R_UNLESS(resource_limit.IsNotNull(), ResultInvalidHandle);

    for (u32 i = 0; i < name_count; ++i) {
        // Get resource limit type.
        const auto name = static_cast<ResourceLimitType>(memory.Read32(names + i * sizeof(u32)));
        R_UNLESS(name < ResourceLimitType::Max, ResultInvalidEnumValue);

        // Write the current value.
        const s64 value = resource_limit->GetCurrentValue(name);
        memory.Write64(values + i * sizeof(u64), value);
    }

    return ResultSuccess;
}

Result SVC::GetResourceLimitLimitValues(VAddr values, Handle resource_limit_handle, VAddr names,
                                        u32 name_count) {
    // Get the handle table.
    auto& handle_table = kernel.GetCurrentProcess()->handle_table;

    // Get the resource limit.
    KScopedAutoObject resource_limit =
        handle_table.GetObject<KResourceLimit>(resource_limit_handle);
    R_UNLESS(resource_limit.IsNotNull(), ResultInvalidHandle);

    for (u32 i = 0; i < name_count; ++i) {
        // Get resource limit type.
        const auto name = static_cast<ResourceLimitType>(memory.Read32(names + i * sizeof(u32)));
        R_UNLESS(name < ResourceLimitType::Max, ResultInvalidEnumValue);

        // Write the limit value.
        const s64 value = resource_limit->GetLimitValue(name);
        memory.Write64(values + i * sizeof(u64), value);
    }

    return ResultSuccess;
}

Result SVC::SetResourceLimitLimitValues(Handle res_limit, VAddr names, VAddr resource_list,
                                        u32 name_count) {
    // Get the handle table.
    auto& handle_table = kernel.GetCurrentProcess()->handle_table;

    // Get the resource limit.
    KScopedAutoObject resource_limit = handle_table.GetObject<KResourceLimit>(res_limit);
    R_UNLESS(resource_limit.IsNotNull(), ResultInvalidHandle);

    for (u32 i = 0; i < name_count; ++i) {
        // Get resource limit type.
        const auto name = static_cast<ResourceLimitType>(memory.Read32(names + i * sizeof(u32)));
        R_UNLESS(name < ResourceLimitType::Max, ResultInvalidEnumValue);

        // Get new resource limit value and validate it.
        const u64 value = memory.Read64(resource_list + i * sizeof(u64));
        const s32 value_high = value >> 32;
        R_UNLESS(value_high >= 0, ResultOutOfRangeKernel);

        // Set the new value.
        if (name == ResourceLimitType::Commit && value_high != 0) {
            auto& config_mem = kernel.GetConfigMemHandler().GetConfigMem();
            config_mem.app_mem_alloc = value_high;
        }
        resource_limit->SetLimitValue(name, static_cast<s32>(value));
    }

    return ResultSuccess;
}

/// Creates a new thread
Result SVC::CreateThread(Handle* out_handle, u32 entry_point, u32 arg, VAddr stack_top,
                         u32 priority, s32 processor_id) {
    static constexpr Result ResultOutOfThreads(ErrCodes::OutOfThreads, ErrorModule::OS,
                                               ErrorSummary::OutOfResource, ErrorLevel::Status);

    // Validate priority.
    R_UNLESS(priority <= ThreadPrioLowest, ResultOutOfRange);

    // Get the current process and handle table.
    const auto process = kernel.GetCurrentProcess();
    auto& handle_table = process->handle_table;

    // Reserve a new address arbiter from the process resource limit.
    KScopedResourceReservation thread_reservation(process, ResourceLimitType::Thread);
    R_UNLESS(thread_reservation.Succeeded(), ResultOutOfThreads);

    // Validate priority limit
    const u32 max_priority = process->resource_limit->GetLimitValue(ResourceLimitType::Priority);
    R_UNLESS(priority >= max_priority || !process->no_thread_restrictions, ResultNotAuthorized);

    // Set the target CPU to the one specified in the process' exheader.
    if (processor_id == ThreadProcessorIdDefault) {
        processor_id = process->ideal_processor;
        ASSERT(processor_id != ThreadProcessorIdDefault);
    }

    // Handle the provided processor id.
    switch (processor_id) {
    case ThreadProcessorId0:
        break;
    case ThreadProcessorIdAll:
        LOG_INFO(Kernel_SVC,
                 "Newly created thread is allowed to be run in any Core, for now run in core 0.");
        processor_id = ThreadProcessorId0;
        break;
    case ThreadProcessorId1:
    case ThreadProcessorId2:
    case ThreadProcessorId3:
        // TODO: Check and log for: When processorid==0x2 and the process is not a BASE mem-region
        // process, exheader kernel-flags bitmask 0x2000 must be set (otherwise error 0xD9001BEA is
        // returned). When processorid==0x3 and the process is not a BASE mem-region process, error
        // 0xD9001BEA is returned. These are the only restriction checks done by the kernel for
        // processorid. If this is implemented, make sure to check process->no_thread_restrictions.
        break;
    default:
        return ResultOutOfRange;
    }

    // Create the address arbiter.
    KThread* thread = KThread::Create(kernel);
    R_UNLESS(thread != nullptr, Result{0xC8601803});

    // Initialize the thread.
    const std::string name = fmt::format("thread-{:08X}", entry_point);
    thread->Initialize(name, entry_point, priority, arg, processor_id, stack_top, process);

    // Commit the reservation.
    thread_reservation.Commit();

    // Register the thread.
    KThread::Register(kernel, thread);

    // Add to the handle table.
    system.PrepareReschedule();
    return handle_table.Add(out_handle, thread);
}

/// Called when a thread exits
void SVC::ExitThread() {
    LOG_TRACE(Kernel_SVC, "called, pc=0x{:08X}", system.GetRunningCore().GetPC());

    kernel.GetCurrentThreadManager().ExitCurrentThread();
    system.PrepareReschedule();
}

Result SVC::GetThreadPriority(u32* out_priority, Handle handle) {
    // Get the thread from its handle.
    KScopedAutoObject thread = kernel.GetCurrentProcess()->handle_table.GetObject<KThread>(handle);
    R_UNLESS(thread.IsNotNull(), ResultInvalidHandle);

    // Get the thread's priority.
    *out_priority = thread->GetPriority();
    return ResultSuccess;
}

/// Sets the priority for the specified thread
Result SVC::SetThreadPriority(Handle thread_handle, u32 priority) {
    // Get the current process.
    Process* process = kernel.GetCurrentProcess();

    // Validate the priority.
    const u32 max_priority = process->resource_limit->GetLimitValue(ResourceLimitType::Priority);
    R_UNLESS(priority <= ThreadPrioLowest, ResultOutOfRange);
    R_UNLESS(priority >= max_priority, ResultNotAuthorized);

    // Get the thread from its handle.
    KScopedAutoObject thread = process->handle_table.GetObject<KThread>(thread_handle);
    R_UNLESS(thread.IsNotNull(), ResultInvalidHandle);

    // Update thread priority.
    thread->SetPriority(priority);
    thread->UpdatePriority();

    // Update the mutexes that this thread is waiting for
    for (KMutex* mutex : thread->m_pending_mutexes) {
        mutex->UpdatePriority();
    }

    system.PrepareReschedule();
    return ResultSuccess;
}

Result SVC::CreateMutex(Handle* out_handle, u32 initial_locked) {
    static constexpr Result ResultOutOfMutexes(ErrCodes::OutOfMutexes, ErrorModule::OS,
                                               ErrorSummary::OutOfResource, ErrorLevel::Status);

    // Get the current process and handle table.
    auto process = kernel.GetCurrentProcess();
    auto& handle_table = process->handle_table;

    // Reserve a new mutex from the process resource limit.
    KScopedResourceReservation mutex_reservation(process, ResourceLimitType::Mutex);
    R_UNLESS(mutex_reservation.Succeeded(), ResultOutOfMutexes);

    // Create the mutex.
    KMutex* mutex = KMutex::Create(kernel);
    R_UNLESS(mutex != nullptr, Result{0xC8601801});

    // Ensure the only reference is in the handle table when we're done.
    SCOPE_EXIT({ mutex->Close(); });

    // Initialize the mutex.
    mutex->Initialize(process, initial_locked);

    // Commit the reservation.
    mutex_reservation.Commit();

    // Register the mutex.
    KMutex::Register(kernel, mutex);

    // Add the mutex to the handle table.
    return handle_table.Add(out_handle, mutex);
}

/// Release a mutex
Result SVC::ReleaseMutex(Handle handle) {
    // Get the handle table.
    auto& handle_table = kernel.GetCurrentProcess()->handle_table;

    // Get the address arbiter.
    KScopedAutoObject mutex = handle_table.GetObject<KMutex>(handle);
    R_UNLESS(mutex.IsNotNull(), ResultInvalidHandle);

    // Release the mutex
    KThread* thread = kernel.GetCurrentThreadManager().GetCurrentThread();
    return mutex->Release(thread);
}

/// Get the ID of the specified process
Result SVC::GetProcessId(u32* process_id, Handle process_handle) {
    // Get the handle table.
    auto& handle_table = kernel.GetCurrentProcess()->handle_table;

    // Get the address arbiter.
    KScopedAutoObject process = handle_table.GetObject<Process>(process_handle);
    R_UNLESS(process.IsNotNull(), ResultInvalidHandle);

    // Retrieve process id.
    *process_id = process->process_id;
    return ResultSuccess;
}

/// Get the ID of the process that owns the specified thread
Result SVC::GetProcessIdOfThread(u32* process_id, Handle thread_handle) {
    // Get the handle table.
    auto& handle_table = kernel.GetCurrentProcess()->handle_table;

    // Get the address arbiter.
    KScopedAutoObject thread = handle_table.GetObject<KThread>(thread_handle);
    R_UNLESS(thread.IsNotNull(), ResultInvalidHandle);

    // Retrieve process id of the thread.
    Process* owner = thread->GetOwner();
    *process_id = owner ? owner->process_id : -1;
    return ResultSuccess;
}

/// Get the ID for the specified thread.
Result SVC::GetThreadId(u32* thread_id, Handle thread_handle) {
    // Get the handle table.
    auto& handle_table = kernel.GetCurrentProcess()->handle_table;

    // Get the address arbiter.
    KScopedAutoObject thread = handle_table.GetObject<KThread>(thread_handle);
    R_UNLESS(thread.IsNotNull(), ResultInvalidHandle);

    // Retrieve thread id.
    *thread_id = thread->GetThreadId();
    return ResultSuccess;
}

/// Creates a semaphore
Result SVC::CreateSemaphore(Handle* out_handle, s32 initial_count, s32 max_count) {
    static constexpr Result ResultOutOfSemaphores(ErrCodes::OutOfSemaphores, ErrorModule::OS,
                                                  ErrorSummary::OutOfResource, ErrorLevel::Status);

    // Get the current process and handle table.
    auto process = kernel.GetCurrentProcess();
    auto& handle_table = process->handle_table;

    // Reserve a new semaphore from the process resource limit.
    KScopedResourceReservation semaphore_reservation(process, ResourceLimitType::Semaphore);
    R_UNLESS(semaphore_reservation.Succeeded(), ResultOutOfSemaphores);

    // Create the semaphore.
    KSemaphore* semaphore = KSemaphore::Create(kernel);
    R_UNLESS(semaphore != nullptr, Result{0xC8601801});

    // Ensure the only reference is in the handle table when we're done.
    SCOPE_EXIT({ semaphore->Close(); });

    // Initialize the semaphore.
    const auto name = fmt::format("semaphore-{:08x}", system.GetRunningCore().GetReg(14));
    semaphore->Initialize(process, initial_count, max_count, name);

    // Commit the reservation.
    semaphore_reservation.Commit();

    // Register the semaphore.
    KSemaphore::Register(kernel, semaphore);

    // Add the semaphore to the handle table.
    return handle_table.Add(out_handle, semaphore);
}

/// Releases a certain number of slots in a semaphore
Result SVC::ReleaseSemaphore(s32* count, Handle handle, s32 release_count) {
    // Get the handle table.
    auto& handle_table = kernel.GetCurrentProcess()->handle_table;

    // Get the semaphore.
    KScopedAutoObject semaphore = handle_table.GetObject<KSemaphore>(handle);
    R_UNLESS(semaphore.IsNotNull(), ResultInvalidHandle);

    // Release the semaphore
    return semaphore->Release(count, release_count);
}

/// Sets the kernel state
Result SVC::KernelSetState(u32 kernel_state, u32 varg1, u32 varg2) {
    switch (static_cast<KernelState>(kernel_state)) {
    // This triggers a hardware reboot on real console, since this doesn't make sense
    // on emulator, we shutdown instead.
    case KernelState::KERNEL_STATE_REBOOT:
        system.RequestShutdown();
        break;

    // Citra specific states.
    case KernelState::KERNEL_STATE_CITRA_EMULATION_SPEED: {
        u16 new_value = static_cast<u16>(varg1);
        Settings::values.frame_limit.SetValue(new_value);
    } break;
    default:
        LOG_ERROR(Kernel_SVC, "Unknown KernelSetState state={} varg1={} varg2={}", kernel_state,
                  varg1, varg2);
    }
    return ResultSuccess;
}

/// Query process memory
Result SVC::QueryProcessMemory(MemoryInfo* memory_info, PageInfo* page_info, Handle process_handle,
                               u32 addr) {
    // Get the handle table.
    auto& handle_table = kernel.GetCurrentProcess()->handle_table;

    // Get the address arbiter.
    KScopedAutoObject process = handle_table.GetObject<Process>(process_handle);
    R_UNLESS(process.IsNotNull(), ResultInvalidHandle);

    // Ensure a VMA for the provided address exists.
    auto& vm_manager = process->vm_manager;
    auto vma = vm_manager.FindVMA(addr);
    R_UNLESS(vma != vm_manager.vma_map.end(), ResultInvalidAddress);

    auto permissions = vma->second.permissions;
    auto state = vma->second.meminfo_state;

    // Query(Process)Memory merges vma with neighbours when they share the same state and
    // permissions, regardless of their physical mapping.

    auto mismatch = [permissions, state](const std::pair<VAddr, Kernel::VirtualMemoryArea>& v) {
        return v.second.permissions != permissions || v.second.meminfo_state != state;
    };

    std::reverse_iterator rvma(vma);

    auto lower = std::find_if(rvma, process->vm_manager.vma_map.crend(), mismatch);
    --lower;
    auto upper = std::find_if(vma, process->vm_manager.vma_map.cend(), mismatch);
    --upper;

    memory_info->base_address = lower->second.base;
    memory_info->permission = static_cast<u32>(permissions);
    memory_info->size = upper->second.base + upper->second.size - lower->second.base;
    memory_info->state = static_cast<u32>(state);

    page_info->flags = 0;
    LOG_TRACE(Kernel_SVC, "called process=0x{:08X} addr=0x{:08X}", process_handle, addr);
    return ResultSuccess;
}

/// Query memory
Result SVC::QueryMemory(MemoryInfo* memory_info, PageInfo* page_info, u32 addr) {
    return QueryProcessMemory(memory_info, page_info, KernelHandle::CurrentProcess, addr);
}

/// Create an event
Result SVC::CreateEvent(Handle* out_handle, u32 reset_type) {
    static constexpr Result ResultOutOfEvents(ErrCodes::OutOfEvents, ErrorModule::OS,
                                              ErrorSummary::OutOfResource, ErrorLevel::Status);

    // Get the current process and handle table.
    auto process = kernel.GetCurrentProcess();
    auto& handle_table = process->handle_table;

    // Reserve a new event from the process resource limit.
    KScopedResourceReservation event_reservation(process, ResourceLimitType::Event);
    R_UNLESS(event_reservation.Succeeded(), ResultOutOfEvents);

    // Create the event.
    KEvent* event = KEvent::Create(kernel);
    R_UNLESS(event != nullptr, Result{0xC8601801});

    // Ensure the only reference is in the handle table when we're done.
    SCOPE_EXIT({ event->Close(); });

    // Initialize the event.
    event->Initialize(process, static_cast<ResetType>(reset_type));

    // Commit the event.
    event_reservation.Commit();

    // Register the event.
    KEvent::Register(kernel, event);

    // Add the event to the handle table.
    return handle_table.Add(out_handle, event);
}

/// Duplicates a kernel handle
Result SVC::DuplicateHandle(Handle* out, Handle handle) {
    // Get the handle table.
    auto& handle_table = kernel.GetCurrentProcess()->handle_table;

    // Get the provided object
    KScopedAutoObject object = handle_table.GetObject<KAutoObject>(handle);
    R_UNLESS(object.IsNotNull(), ResultInvalidHandle);

    // Add it to the handle table again.
    return handle_table.Add(out, object.GetPointerUnsafe());
}

/// Signals an event
Result SVC::SignalEvent(Handle handle) {
    // Get the handle table.
    auto& handle_table = kernel.GetCurrentProcess()->handle_table;

    // Get the event.
    KScopedAutoObject event = handle_table.GetObject<KEvent>(handle);
    R_UNLESS(event.IsNotNull(), ResultInvalidHandle);

    // Signal the event
    event->Signal();
    return ResultSuccess;
}

/// Clears an event
Result SVC::ClearEvent(Handle handle) {
    // Get the handle table.
    auto& handle_table = kernel.GetCurrentProcess()->handle_table;

    // Get the event.
    KScopedAutoObject event = handle_table.GetObject<KEvent>(handle);
    R_UNLESS(event.IsNotNull(), ResultInvalidHandle);

    // Clear the event
    event->Clear();
    return ResultSuccess;
}

/// Creates a timer
Result SVC::CreateTimer(Handle* out_handle, u32 reset_type) {
    static constexpr Result ResultOutOfTimers(ErrCodes::OutOfTimers, ErrorModule::OS,
                                              ErrorSummary::OutOfResource, ErrorLevel::Status);

    // Get the current process and handle table.
    auto process = kernel.GetCurrentProcess();
    auto& handle_table = process->handle_table;

    // Reserve a new event from the process resource limit.
    KScopedResourceReservation timer_reservation(process, ResourceLimitType::Timer);
    R_UNLESS(timer_reservation.Succeeded(), ResultOutOfTimers);

    // Create the event.
    KTimer* timer = KTimer::Create(kernel);
    R_UNLESS(timer != nullptr, Result{0xC8601801});

    // Ensure the only reference is in the handle table when we're done.
    SCOPE_EXIT({ timer->Close(); });

    // Initialize the event.
    timer->Initialize(process, static_cast<ResetType>(reset_type));

    // Commit the event.
    timer_reservation.Commit();

    // Register the event.
    KTimer::Register(kernel, timer);

    // Add the event to the handle table.
    return handle_table.Add(out_handle, timer);
}

/// Clears a timer
Result SVC::ClearTimer(Handle handle) {
    // Get the handle table.
    auto& handle_table = kernel.GetCurrentProcess()->handle_table;

    // Get the timer.
    KScopedAutoObject timer = handle_table.GetObject<KTimer>(handle);
    R_UNLESS(timer.IsNotNull(), ResultInvalidHandle);

    // Clear the timer
    timer->Clear();
    return ResultSuccess;
}

/// Starts a timer
Result SVC::SetTimer(Handle handle, s64 initial, s64 interval) {
    // Validate interval
    R_UNLESS(initial >= 0 && interval >= 0, ResultOutOfRangeKernel);

    // Get the handle table.
    auto& handle_table = kernel.GetCurrentProcess()->handle_table;

    // Get the timer.
    KScopedAutoObject timer = handle_table.GetObject<KTimer>(handle);
    R_UNLESS(timer.IsNotNull(), ResultInvalidHandle);

    // Set the timer
    timer->Set(initial, interval);
    return ResultSuccess;
}

/// Cancels a timer
Result SVC::CancelTimer(Handle handle) {
    // Get the handle table.
    auto& handle_table = kernel.GetCurrentProcess()->handle_table;

    // Get the timer.
    KScopedAutoObject timer = handle_table.GetObject<KTimer>(handle);
    R_UNLESS(timer.IsNotNull(), ResultInvalidHandle);

    // Cancel the timer
    timer->Cancel();
    return ResultSuccess;
}

/// Sleep the current thread
void SVC::SleepThread(s64 nanoseconds) {
    LOG_TRACE(Kernel_SVC, "called nanoseconds={}", nanoseconds);

    ThreadManager& thread_manager = kernel.GetCurrentThreadManager();

    // Don't attempt to yield execution if there are no available threads to run,
    // this way we avoid a useless reschedule to the idle thread.
    if (nanoseconds == 0 && !thread_manager.HaveReadyThreads()) {
        return;
    }

    // Sleep current thread and check for next thread to schedule
    thread_manager.WaitCurrentThread_Sleep();

    // Create an event to wake the thread up after the specified nanosecond delay has passed
    thread_manager.GetCurrentThread()->WakeAfterDelay(nanoseconds);

    system.PrepareReschedule();
}

/// This returns the total CPU ticks elapsed since the CPU was powered-on
s64 SVC::GetSystemTick() {
    // TODO: Use globalTicks here?
    s64 result = system.GetRunningCore().GetTimer().GetTicks();
    // Advance time to defeat dumb games (like Cubic Ninja) that busy-wait for the frame to end.
    // Measured time between two calls on a 9.2 o3DS with Ninjhax 1.1b
    system.GetRunningCore().GetTimer().AddTicks(150);
    return result;
}

/// Returns information of the specified handle
Result SVC::GetHandleInfo(s64* out, Handle handle, u32 type) {
    // Get the object.
    KScopedAutoObject object =
        kernel.GetCurrentProcess()->handle_table.GetObject<KAutoObject>(handle);
    R_UNLESS(object.IsNotNull(), ResultInvalidHandle);

    // Not initialized in real kernel, but we don't want to leak memory.
    s64 value = 0;

    switch (static_cast<HandleInfoType>(type)) {
    case HandleInfoType::KPROCESS_ELAPSED_TICKS: {
        Process* process = object->DynamicCast<Process*>();
        if (process) {
            value = process->creation_time_ticks;
        }
        break;
    }
    case HandleInfoType::REFERENCE_COUNT:
        value = object->GetReferenceCount();
        break;
    // These values are stubbed in real kernel, they do nothing.
    case HandleInfoType::STUBBED_1:
    case HandleInfoType::STUBBED_2:
        break;
    default:
        return ResultInvalidEnumValue;
    }

    // Write the result.
    *out = value;
    return ResultSuccess;
}

/// Creates a memory block at the specified address with the specified permissions and size
Result SVC::CreateMemoryBlock(Handle* out_handle, u32 addr, u32 size, u32 my_permission,
                              u32 other_permission) {
    static constexpr Result ResultOutOfSharedMems(ErrCodes::OutOfSharedMems, ErrorModule::OS,
                                                  ErrorSummary::OutOfResource, ErrorLevel::Status);

    const auto IsNotExecute = [](u32 permission) {
        const auto perm = static_cast<MemoryPermission>(permission);
        switch (perm) {
        case MemoryPermission::None:
        case MemoryPermission::Read:
        case MemoryPermission::Write:
        case MemoryPermission::ReadWrite:
        case MemoryPermission::DontCare:
            return true;
        default:
            return false;
        }
    };

    // Get current process and handle table.
    Process* process = kernel.GetCurrentProcess();
    auto& handle_table = process->handle_table;

    // Validate the size alignment.
    R_UNLESS(Common::Is4KBAligned(size), ResultMisalignedSize);

    // SharedMemory blocks can not be created with Execute permissions
    R_UNLESS(IsNotExecute(my_permission) && IsNotExecute(other_permission),
             ResultInvalidCombination);

    // Processes with memory type APPLICATION are not allowed to create memory blocks with addr = 0
    R_UNLESS(process->flags.memory_region != MemoryRegion::APPLICATION || addr != 0,
             Result{0xD92007EA});
    R_UNLESS(
        (addr >= Memory::PROCESS_IMAGE_VADDR && addr + size <= Memory::SHARED_MEMORY_VADDR_END) ||
            addr == 0,
        ResultInvalidAddress);

    // Reserve a new shared memory from the process resource limit.
    KScopedResourceReservation shared_mem_reservation(process, ResourceLimitType::SharedMemory);
    R_UNLESS(shared_mem_reservation.Succeeded(), ResultOutOfSharedMems);

    // When trying to create a memory block with address = 0,
    // if the process has the Shared Device Memory flag in the exheader,
    // then we have to allocate from the same region as the caller process instead of the BASE
    // region.
    const MemoryRegion region = [&] {
        if (addr == 0 && process->flags.shared_device_mem) {
            return static_cast<MemoryRegion>(process->flags.memory_region);
        }
        return MemoryRegion::BASE;
    }();

    // Create the shared memory.
    KSharedMemory* shared_memory = KSharedMemory::Create(kernel);
    R_UNLESS(shared_memory != nullptr, Result{0xC8601802});

    // Ensure the only reference is in the handle table when we're done.
    SCOPE_EXIT({ shared_memory->Close(); });

    // Initialize the shared memory.
    const auto my_perm = static_cast<MemoryPermission>(my_permission);
    const auto other_perm = static_cast<MemoryPermission>(other_permission);
    R_TRY(shared_memory->Initialize(process, size, my_perm, other_perm, addr, region));

    // Commit the shared memory.
    shared_mem_reservation.Commit();

    // Register the shared memory.
    KSharedMemory::Register(kernel, shared_memory);

    // Add the shared memory to the handle table.
    return handle_table.Add(out_handle, shared_memory);
}

Result SVC::CreatePort(Handle* server_port, Handle* client_port, VAddr name_address,
                       u32 max_sessions) {
    // Copy the provided name from user memory to kernel memory.
    const auto string_name = memory.ReadCString(name_address, KObjectName::NameLengthMax);

    std::array<char, KObjectName::NameLengthMax> name{};
    std::strncpy(name.data(), string_name.c_str(), KObjectName::NameLengthMax - 1);

    // Validate that the name is valid.
    R_UNLESS(name[sizeof(name) - 1] == '\x00', ResultPortNameTooLong);

    // Get the current handle table.
    auto& handle_table = kernel.GetCurrentProcess()->handle_table;

    // Create the port.
    KPort* port = KPort::Create(kernel);
    R_UNLESS(port, Result{0xC8601808});

    // Initialize the port.
    port->Initialize(max_sessions, "SvcPort");

    // Register the port.
    KPort::Register(kernel, port);

    // Ensure that our only reference to the port is in the handle table when we're done.
    SCOPE_EXIT({
        port->GetClientPort().Close();
        port->GetServerPort().Close();
    });

    // Create a new object name.
    if (name[0]) {
        *client_port = 0;
        return KObjectName::NewFromName(kernel, port, name.data());
    }

    // Add the client and server ports to the handle table.
    // Note: The 3DS kernel also leaks the client port handle
    // if the server port handle fails to be created.
    R_TRY(handle_table.Add(client_port, std::addressof(port->GetClientPort())));
    R_TRY(handle_table.Add(server_port, std::addressof(port->GetServerPort())));
    return ResultSuccess;
}

Result SVC::CreateSessionToPort(Handle* out_client_session, Handle client_port_handle) {
    // Get the current handle table.
    auto& handle_table = kernel.GetCurrentProcess()->handle_table;

    // Get the client port.
    KScopedAutoObject port = handle_table.GetObject<KClientPort>(client_port_handle);
    R_UNLESS(port.IsNotNull(), ResultInvalidHandle);

    // Create a session.
    KClientSession* session;
    R_TRY(port->CreateSession(std::addressof(session)));

    // Close the extra session reference on return.
    SCOPE_EXIT({ session->Close(); });

    // Register the session in the table, close the extra reference.
    return handle_table.Add(out_client_session, session);
}

Result SVC::CreateSession(Handle* server_session, Handle* client_session) {
    // Get the current handle table.
    auto& handle_table = kernel.GetCurrentProcess()->handle_table;

    // Create the session.
    KSession* session = KSession::Create(kernel);
    R_UNLESS(session, Result{0xC8601809});

    // Initialize the port.
    session->Initialize(nullptr);

    // Register the port.
    KSession::Register(kernel, session);

    // Add the client and server session to the handle table.
    R_TRY(handle_table.Add(client_session, std::addressof(session->GetClientSession())));
    R_TRY(handle_table.Add(server_session, std::addressof(session->GetServerSession())));
    return ResultSuccess;
}

Result SVC::AcceptSession(Handle* out_server_session, Handle server_port_handle) {
    // Get the current handle table.
    auto& handle_table = kernel.GetCurrentProcess()->handle_table;

    // Get the server port.
    KScopedAutoObject port = handle_table.GetObject<KServerPort>(server_port_handle);
    R_UNLESS(port.IsNotNull(), ResultInvalidHandle);

    // Accept the session.
    KServerSession* session = port->AcceptSession();
    R_UNLESS(session, ResultNoPendingSessions);

    // Close the extra session reference on return.
    SCOPE_EXIT({ session->Close(); });

    // Add session to the handle table.
    return handle_table.Add(out_server_session, session);
}

static void CopyStringPart(char* out, const char* in, std::size_t offset, std::size_t max_length) {
    std::size_t str_size = strlen(in);
    if (offset < str_size) {
        strncpy(out, in + offset, max_length - 1);
        out[max_length - 1] = '\0';
    } else {
        out[0] = '\0';
    }
}

Result SVC::GetSystemInfo(s64* out, u32 type, s32 param) {
    LOG_TRACE(Kernel_SVC, "called type={} param={}", type, param);

    switch ((SystemInfoType)type) {
    case SystemInfoType::REGION_MEMORY_USAGE:
        switch ((SystemInfoMemUsageRegion)param) {
        case SystemInfoMemUsageRegion::ALL:
            *out = kernel.GetMemoryRegion(MemoryRegion::APPLICATION)->used +
                   kernel.GetMemoryRegion(MemoryRegion::SYSTEM)->used +
                   kernel.GetMemoryRegion(MemoryRegion::BASE)->used;
            break;
        case SystemInfoMemUsageRegion::APPLICATION:
            *out = kernel.GetMemoryRegion(MemoryRegion::APPLICATION)->used;
            break;
        case SystemInfoMemUsageRegion::SYSTEM:
            *out = kernel.GetMemoryRegion(MemoryRegion::SYSTEM)->used;
            break;
        case SystemInfoMemUsageRegion::BASE:
            *out = kernel.GetMemoryRegion(MemoryRegion::BASE)->used;
            break;
        default:
            LOG_ERROR(Kernel_SVC, "unknown GetSystemInfo type=0 region: param={}", param);
            *out = 0;
            break;
        }
        break;
    case SystemInfoType::KERNEL_ALLOCATED_PAGES:
        LOG_ERROR(Kernel_SVC, "unimplemented GetSystemInfo type=2 param={}", param);
        *out = 0;
        break;
    case SystemInfoType::KERNEL_SPAWNED_PIDS:
        *out = 5;
        break;
    case SystemInfoType::NEW_3DS_INFO:
        // The actual subtypes are not implemented, homebrew just check
        // this doesn't return an error in n3ds to know the system type
        LOG_ERROR(Kernel_SVC, "unimplemented GetSystemInfo type=65537 param={}", param);
        *out = 0;
        return (system.GetNumCores() == 4) ? ResultSuccess : ResultInvalidEnumValue;
    case SystemInfoType::CITRA_INFORMATION:
        switch ((SystemInfoCitraInformation)param) {
        case SystemInfoCitraInformation::IS_CITRA:
            *out = 1;
            break;
        case SystemInfoCitraInformation::HOST_TICK:
            *out = static_cast<s64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
            break;
        case SystemInfoCitraInformation::EMULATION_SPEED:
            *out = static_cast<s64>(Settings::values.frame_limit.GetValue());
            break;
        case SystemInfoCitraInformation::BUILD_NAME:
            CopyStringPart(reinterpret_cast<char*>(out), Common::g_build_name, 0, sizeof(s64));
            break;
        case SystemInfoCitraInformation::BUILD_VERSION:
            CopyStringPart(reinterpret_cast<char*>(out), Common::g_build_version, 0, sizeof(s64));
            break;
        case SystemInfoCitraInformation::BUILD_PLATFORM: {
#if defined(_WIN32)
            *out = static_cast<s64>(SystemInfoCitraPlatform::PLATFORM_WINDOWS);
#elif defined(ANDROID)
            *out = static_cast<s64>(SystemInfoCitraPlatform::PLATFORM_ANDROID);
#elif defined(__linux__)
            *out = static_cast<s64>(SystemInfoCitraPlatform::PLATFORM_LINUX);
#elif defined(__APPLE__)
            *out = static_cast<s64>(SystemInfoCitraPlatform::PLATFORM_APPLE);
#else
            *out = static_cast<s64>(SystemInfoCitraPlatform::PLATFORM_UNKNOWN);
#endif
            break;
        }
        case SystemInfoCitraInformation::BUILD_DATE_PART1:
            CopyStringPart(reinterpret_cast<char*>(out), Common::g_build_date,
                           (sizeof(s64) - 1) * 0, sizeof(s64));
            break;
        case SystemInfoCitraInformation::BUILD_DATE_PART2:
            CopyStringPart(reinterpret_cast<char*>(out), Common::g_build_date,
                           (sizeof(s64) - 1) * 1, sizeof(s64));
            break;
        case SystemInfoCitraInformation::BUILD_DATE_PART3:
            CopyStringPart(reinterpret_cast<char*>(out), Common::g_build_date,
                           (sizeof(s64) - 1) * 2, sizeof(s64));
            break;
        case SystemInfoCitraInformation::BUILD_DATE_PART4:
            CopyStringPart(reinterpret_cast<char*>(out), Common::g_build_date,
                           (sizeof(s64) - 1) * 3, sizeof(s64));
            break;
        case SystemInfoCitraInformation::BUILD_GIT_BRANCH_PART1:
            CopyStringPart(reinterpret_cast<char*>(out), Common::g_scm_branch,
                           (sizeof(s64) - 1) * 0, sizeof(s64));
            break;
        case SystemInfoCitraInformation::BUILD_GIT_BRANCH_PART2:
            CopyStringPart(reinterpret_cast<char*>(out), Common::g_scm_branch,
                           (sizeof(s64) - 1) * 1, sizeof(s64));
            break;
        case SystemInfoCitraInformation::BUILD_GIT_DESCRIPTION_PART1:
            CopyStringPart(reinterpret_cast<char*>(out), Common::g_scm_desc, (sizeof(s64) - 1) * 0,
                           sizeof(s64));
            break;
        case SystemInfoCitraInformation::BUILD_GIT_DESCRIPTION_PART2:
            CopyStringPart(reinterpret_cast<char*>(out), Common::g_scm_desc, (sizeof(s64) - 1) * 1,
                           sizeof(s64));
            break;
        default:
            LOG_ERROR(Kernel_SVC, "unknown GetSystemInfo citra info param={}", param);
            *out = 0;
            break;
        }
        break;
    default:
        LOG_ERROR(Kernel_SVC, "unknown GetSystemInfo type={} param={}", type, param);
        *out = 0;
        break;
    }

    // This function never returns an error, even if invalid parameters were passed.
    return ResultSuccess;
}

Result SVC::GetProcessInfo(s64* out, Handle process_handle, u32 type) {
    LOG_TRACE(Kernel_SVC, "called process=0x{:08X} type={}", process_handle, type);

    // Get the process.
    KScopedAutoObject process =
        kernel.GetCurrentProcess()->handle_table.GetObject<Process>(process_handle);
    R_UNLESS(process.IsNotNull(), ResultInvalidHandle);

    switch (static_cast<ProcessInfoType>(type)) {
    case ProcessInfoType::PRIVATE_AND_SHARED_USED_MEMORY:
    case ProcessInfoType::PRIVATE_SHARED_SUPERVISOR_HANDLE_USED_MEMORY:
        // TODO(yuriks): Type 0 returns a slightly higher number than type 2, but I'm not sure
        // what's the difference between them.
        *out = process->memory_used;
        if (*out % Memory::CITRA_PAGE_SIZE != 0) {
            LOG_ERROR(Kernel_SVC, "called, memory size not page-aligned");
            return ResultMisalignedSize;
        }
        break;
    case ProcessInfoType::SUPERVISOR_AND_HANDLE_USED_MEMORY:
    case ProcessInfoType::SUPERVISOR_AND_HANDLE_USED_MEMORY2:
    case ProcessInfoType::USED_HANDLE_COUNT:
    case ProcessInfoType::HIGHEST_HANDLE_COUNT:
    case ProcessInfoType::KPROCESS_0X234:
    case ProcessInfoType::THREAD_COUNT:
    case ProcessInfoType::MAX_THREAD_AMOUNT:
        // These are valid, but not implemented yet
        LOG_ERROR(Kernel_SVC, "unimplemented GetProcessInfo type={}", type);
        break;
    case ProcessInfoType::LINEAR_BASE_ADDR_OFFSET:
        *out = Memory::FCRAM_PADDR - process->GetLinearHeapAreaAddress();
        break;
    case ProcessInfoType::QTM_MEMORY_BLOCK_CONVERSION_OFFSET:
    case ProcessInfoType::QTM_MEMORY_ADDRESS:
    case ProcessInfoType::QTM_MEMORY_SIZE:
        // These return a different error value than higher invalid values
        LOG_ERROR(Kernel_SVC, "unknown GetProcessInfo type={}", type);
        return ResultNotImplemented;
    // Here start the custom ones, taken from Luma3DS for 3GX support
    case ProcessInfoType::LUMA_CUSTOM_PROCESS_NAME:
        // Get process name
        std::strncpy(reinterpret_cast<char*>(out), process->codeset.name.c_str(), 8);
        break;
    case ProcessInfoType::LUMA_CUSTOM_PROCESS_TITLE_ID:
        // Get process TID
        *out = process->codeset.program_id;
        break;
    case ProcessInfoType::LUMA_CUSTOM_TEXT_SIZE:
        *out = process->codeset.CodeSegment().size;
        break;
    case ProcessInfoType::LUMA_CUSTOM_RODATA_SIZE:
        *out = process->codeset.RODataSegment().size;
        break;
    case ProcessInfoType::LUMA_CUSTOM_DATA_SIZE:
        *out = process->codeset.DataSegment().size;
        break;
    case ProcessInfoType::LUMA_CUSTOM_TEXT_ADDR:
        *out = process->codeset.CodeSegment().addr;
        break;
    case ProcessInfoType::LUMA_CUSTOM_RODATA_ADDR:
        *out = process->codeset.RODataSegment().addr;
        break;
    case ProcessInfoType::LUMA_CUSTOM_DATA_ADDR:
        *out = process->codeset.DataSegment().addr;
        break;

    default:
        LOG_ERROR(Kernel_SVC, "unknown GetProcessInfo type={}", type);
        return ResultInvalidEnumValue;
    }

    return ResultSuccess;
}

Result SVC::GetThreadInfo(s64* out, Handle thread_handle, u32 type) {
    LOG_TRACE(Kernel_SVC, "called thread=0x{:08X} type={}", thread_handle, type);

    // Get the thread.
    KScopedAutoObject thread =
        kernel.GetCurrentProcess()->handle_table.GetObject<KThread>(thread_handle);
    R_UNLESS(thread.IsNotNull(), ResultInvalidHandle);

    switch (type) {
    case 0x10000:
        *out = static_cast<s64>(thread->GetTLSAddress());
        break;
    default:
        LOG_ERROR(Kernel_SVC, "unknown GetThreadInfo type={}", type);
        return ResultInvalidEnumValue;
    }

    return ResultSuccess;
}

Result SVC::GetProcessList(s32* process_count, VAddr out_process_array,
                           s32 out_process_array_count) {
    // Validate virtual address
    Process* current_process = kernel.GetCurrentProcess();
    R_UNLESS(memory.IsValidVirtualAddress(*current_process, out_process_array),
             ResultInvalidPointer);

    // Retrieve process list.
    s32 written = 0;
    for (const Process* process : kernel.GetProcessList()) {
        if (written >= out_process_array_count) {
            break;
        }
        if (process) {
            memory.Write32(out_process_array + written++ * sizeof(u32), process->process_id);
        }
    }

    *process_count = written;
    return ResultSuccess;
}

Result SVC::InvalidateInstructionCacheRange(u32 addr, u32 size) {
    system.GetRunningCore().InvalidateCacheRange(addr, size);
    return ResultSuccess;
}

Result SVC::InvalidateEntireInstructionCache() {
    system.GetRunningCore().ClearInstructionCache();
    return ResultSuccess;
}

u32 SVC::ConvertVaToPa(u32 addr) {
    auto& vm_manager = kernel.GetCurrentProcess()->vm_manager;
    auto vma = vm_manager.FindVMA(addr);
    if (vma == vm_manager.vma_map.end() || vma->second.type != VMAType::BackingMemory) {
        return 0;
    }
    const u8* backing_mem = vma->second.backing_memory.GetPtr();
    return kernel.memory.GetFCRAMOffset(backing_mem + addr - vma->second.base) +
           Memory::FCRAM_PADDR;
}

Result SVC::MapProcessMemoryEx(Handle dst_process_handle, u32 dst_address,
                               Handle src_process_handle, u32 src_address, u32 size) {
    // Get handle table
    auto& handle_table = kernel.GetCurrentProcess()->handle_table;

    // Get the source processs.
    KScopedAutoObject src_process = handle_table.GetObject<Process>(src_process_handle);
    R_UNLESS(src_process.IsNotNull(), ResultInvalidHandle);

    // Get the destination processs.
    KScopedAutoObject dst_process = handle_table.GetObject<Process>(dst_process_handle);
    R_UNLESS(dst_process.IsNotNull(), ResultInvalidHandle);

    // Align size to page size
    size = Common::AlignUp(size, Memory::CITRA_PAGE_SIZE);

    // Only linear memory supported
    auto src_vma = src_process->vm_manager.FindVMA(src_address);
    if (src_vma == src_process->vm_manager.vma_map.end() ||
        src_vma->second.type != VMAType::BackingMemory ||
        src_vma->second.meminfo_state != MemoryState::Continuous) {
        return ResultInvalidAddress;
    }

    // Validate offset is within bounds
    const u32 offset = src_address - src_vma->second.base;
    R_UNLESS(offset + size <= src_vma->second.size, ResultInvalidAddress);

    auto vma_res = dst_process->vm_manager.MapBackingMemory(
        dst_address,
        memory.GetFCRAMRef(src_vma->second.backing_memory.GetPtr() + offset -
                           kernel.memory.GetFCRAMPointer(0)),
        size, Kernel::MemoryState::Continuous);

    R_UNLESS(vma_res.Succeeded(), ResultInvalidAddressState);
    dst_process->vm_manager.Reprotect(vma_res.Unwrap(), Kernel::VMAPermission::ReadWriteExecute);

    return ResultSuccess;
}

Result SVC::UnmapProcessMemoryEx(Handle process_handle, u32 dst_address, u32 size) {
    // Get the destination processs.
    KScopedAutoObject process =
        kernel.GetCurrentProcess()->handle_table.GetObject<Process>(process_handle);
    R_UNLESS(process.IsNotNull(), ResultInvalidHandle);

    // Align size to page size
    size = Common::AlignUp(size, Memory::CITRA_PAGE_SIZE);

    // Only linear memory supported
    auto vma = process->vm_manager.FindVMA(dst_address);
    if (vma == process->vm_manager.vma_map.end() || vma->second.type != VMAType::BackingMemory ||
        vma->second.meminfo_state != MemoryState::Continuous) {
        return ResultInvalidAddress;
    }

    // Unmap
    process->vm_manager.UnmapRange(dst_address, size);
    return ResultSuccess;
}

Result SVC::ControlProcess(Handle process_handle, u32 process_OP, u32 varg2, u32 varg3) {
    // Get the destination processs.
    KScopedAutoObject process =
        kernel.GetCurrentProcess()->handle_table.GetObject<Process>(process_handle);
    R_UNLESS(process.IsNotNull(), ResultInvalidHandle);

    switch (static_cast<ControlProcessOP>(process_OP)) {
    case ControlProcessOP::PROCESSOP_SET_MMU_TO_RWX: {
        auto& vm_manager = process->vm_manager;
        for (auto it = vm_manager.vma_map.cbegin(); it != vm_manager.vma_map.cend(); it++) {
            if (it->second.meminfo_state != MemoryState::Free) {
                process->vm_manager.Reprotect(it, Kernel::VMAPermission::ReadWriteExecute);
            }
        }
        return ResultSuccess;
    }
    case ControlProcessOP::PROCESSOP_GET_ON_MEMORY_CHANGE_EVENT: {
        // Get plgldr service
        auto plgldr = Service::PLGLDR::GetService(system);
        R_UNLESS(plgldr, ResultNotFound);

        // Get memory changed handle.
        const auto out = plgldr->GetMemoryChangedHandle(kernel);
        R_UNLESS(out.Succeeded(), out.Code());

        // Write it.
        memory.Write32(varg2, out.Unwrap());
        return ResultSuccess;
    }
    case ControlProcessOP::PROCESSOP_SCHEDULE_THREADS_WITHOUT_TLS_MAGIC: {
        for (u32 core_id = 0; core_id < system.GetNumCores(); core_id++) {
            const auto thread_list = kernel.GetThreadManager(core_id).GetThreadList();
            for (auto& thread : thread_list) {
                if (thread->GetOwner() != process.GetPointerUnsafe()) {
                    continue;
                }
                if (memory.Read32(thread->GetTLSAddress()) == varg3) {
                    continue;
                }
                KThread* current = kernel.GetCurrentThreadManager().GetCurrentThread();
                if (thread->GetThreadId() == current->GetThreadId()) {
                    continue;
                }
                thread->m_can_schedule = !varg2;
            }
        }
        return ResultSuccess;
    }
    case ControlProcessOP::PROCESSOP_DISABLE_CREATE_THREAD_RESTRICTIONS: {
        process->no_thread_restrictions = varg2 == 1;
        return ResultSuccess;
    }
    case ControlProcessOP::PROCESSOP_GET_ALL_HANDLES:
    case ControlProcessOP::PROCESSOP_GET_PA_FROM_VA:
    case ControlProcessOP::PROCESSOP_SIGNAL_ON_EXIT:
    case ControlProcessOP::PROCESSOP_SCHEDULE_THREADS:
    default:
        LOG_ERROR(Kernel_SVC, "Unknown ControlProcessOp type={}", process_OP);
        return ResultNotImplemented;
    }
}

const std::array<SVC::FunctionDef, 180> SVC::SVC_Table{{
    {0x00, nullptr, "Unknown"},
    {0x01, &SVC::Wrap<&SVC::ControlMemory>, "ControlMemory"},
    {0x02, &SVC::Wrap<&SVC::QueryMemory>, "QueryMemory"},
    {0x03, &SVC::ExitProcess, "ExitProcess"},
    {0x04, nullptr, "GetProcessAffinityMask"},
    {0x05, nullptr, "SetProcessAffinityMask"},
    {0x06, nullptr, "GetProcessIdealProcessor"},
    {0x07, nullptr, "SetProcessIdealProcessor"},
    {0x08, &SVC::Wrap<&SVC::CreateThread>, "CreateThread"},
    {0x09, &SVC::ExitThread, "ExitThread"},
    {0x0A, &SVC::Wrap<&SVC::SleepThread>, "SleepThread"},
    {0x0B, &SVC::Wrap<&SVC::GetThreadPriority>, "GetThreadPriority"},
    {0x0C, &SVC::Wrap<&SVC::SetThreadPriority>, "SetThreadPriority"},
    {0x0D, nullptr, "GetThreadAffinityMask"},
    {0x0E, nullptr, "SetThreadAffinityMask"},
    {0x0F, nullptr, "GetThreadIdealProcessor"},
    {0x10, nullptr, "SetThreadIdealProcessor"},
    {0x11, nullptr, "GetCurrentProcessorNumber"},
    {0x12, nullptr, "Run"},
    {0x13, &SVC::Wrap<&SVC::CreateMutex>, "CreateMutex"},
    {0x14, &SVC::Wrap<&SVC::ReleaseMutex>, "ReleaseMutex"},
    {0x15, &SVC::Wrap<&SVC::CreateSemaphore>, "CreateSemaphore"},
    {0x16, &SVC::Wrap<&SVC::ReleaseSemaphore>, "ReleaseSemaphore"},
    {0x17, &SVC::Wrap<&SVC::CreateEvent>, "CreateEvent"},
    {0x18, &SVC::Wrap<&SVC::SignalEvent>, "SignalEvent"},
    {0x19, &SVC::Wrap<&SVC::ClearEvent>, "ClearEvent"},
    {0x1A, &SVC::Wrap<&SVC::CreateTimer>, "CreateTimer"},
    {0x1B, &SVC::Wrap<&SVC::SetTimer>, "SetTimer"},
    {0x1C, &SVC::Wrap<&SVC::CancelTimer>, "CancelTimer"},
    {0x1D, &SVC::Wrap<&SVC::ClearTimer>, "ClearTimer"},
    {0x1E, &SVC::Wrap<&SVC::CreateMemoryBlock>, "CreateMemoryBlock"},
    {0x1F, &SVC::Wrap<&SVC::MapMemoryBlock>, "MapMemoryBlock"},
    {0x20, &SVC::Wrap<&SVC::UnmapMemoryBlock>, "UnmapMemoryBlock"},
    {0x21, &SVC::Wrap<&SVC::CreateAddressArbiter>, "CreateAddressArbiter"},
    {0x22, &SVC::Wrap<&SVC::ArbitrateAddress>, "ArbitrateAddress"},
    {0x23, &SVC::Wrap<&SVC::CloseHandle>, "CloseHandle"},
    {0x24, &SVC::Wrap<&SVC::WaitSynchronization1>, "WaitSynchronization1"},
    {0x25, &SVC::Wrap<&SVC::WaitSynchronizationN>, "WaitSynchronizationN"},
    {0x26, nullptr, "SignalAndWait"},
    {0x27, &SVC::Wrap<&SVC::DuplicateHandle>, "DuplicateHandle"},
    {0x28, &SVC::Wrap<&SVC::GetSystemTick>, "GetSystemTick"},
    {0x29, &SVC::Wrap<&SVC::GetHandleInfo>, "GetHandleInfo"},
    {0x2A, &SVC::Wrap<&SVC::GetSystemInfo>, "GetSystemInfo"},
    {0x2B, &SVC::Wrap<&SVC::GetProcessInfo>, "GetProcessInfo"},
    {0x2C, &SVC::Wrap<&SVC::GetThreadInfo>, "GetThreadInfo"},
    {0x2D, &SVC::Wrap<&SVC::ConnectToPort>, "ConnectToPort"},
    {0x2E, nullptr, "SendSyncRequest1"},
    {0x2F, nullptr, "SendSyncRequest2"},
    {0x30, nullptr, "SendSyncRequest3"},
    {0x31, nullptr, "SendSyncRequest4"},
    {0x32, &SVC::Wrap<&SVC::SendSyncRequest>, "SendSyncRequest"},
    {0x33, &SVC::Wrap<&SVC::OpenProcess>, "OpenProcess"},
    {0x34, &SVC::Wrap<&SVC::OpenThread>, "OpenThread"},
    {0x35, &SVC::Wrap<&SVC::GetProcessId>, "GetProcessId"},
    {0x36, &SVC::Wrap<&SVC::GetProcessIdOfThread>, "GetProcessIdOfThread"},
    {0x37, &SVC::Wrap<&SVC::GetThreadId>, "GetThreadId"},
    {0x38, &SVC::Wrap<&SVC::GetResourceLimit>, "GetResourceLimit"},
    {0x39, &SVC::Wrap<&SVC::GetResourceLimitLimitValues>, "GetResourceLimitLimitValues"},
    {0x3A, &SVC::Wrap<&SVC::GetResourceLimitCurrentValues>, "GetResourceLimitCurrentValues"},
    {0x3B, nullptr, "GetThreadContext"},
    {0x3C, &SVC::Wrap<&SVC::Break>, "Break"},
    {0x3D, &SVC::Wrap<&SVC::OutputDebugString>, "OutputDebugString"},
    {0x3E, nullptr, "ControlPerformanceCounter"},
    {0x3F, nullptr, "Unknown"},
    {0x40, nullptr, "Unknown"},
    {0x41, nullptr, "Unknown"},
    {0x42, nullptr, "Unknown"},
    {0x43, nullptr, "Unknown"},
    {0x44, nullptr, "Unknown"},
    {0x45, nullptr, "Unknown"},
    {0x46, nullptr, "Unknown"},
    {0x47, &SVC::Wrap<&SVC::CreatePort>, "CreatePort"},
    {0x48, &SVC::Wrap<&SVC::CreateSessionToPort>, "CreateSessionToPort"},
    {0x49, &SVC::Wrap<&SVC::CreateSession>, "CreateSession"},
    {0x4A, &SVC::Wrap<&SVC::AcceptSession>, "AcceptSession"},
    {0x4B, nullptr, "ReplyAndReceive1"},
    {0x4C, nullptr, "ReplyAndReceive2"},
    {0x4D, nullptr, "ReplyAndReceive3"},
    {0x4E, nullptr, "ReplyAndReceive4"},
    {0x4F, &SVC::Wrap<&SVC::ReplyAndReceive>, "ReplyAndReceive"},
    {0x50, nullptr, "BindInterrupt"},
    {0x51, nullptr, "UnbindInterrupt"},
    {0x52, &SVC::Wrap<&SVC::InvalidateProcessDataCache>, "InvalidateProcessDataCache"},
    {0x53, &SVC::Wrap<&SVC::StoreProcessDataCache>, "StoreProcessDataCache"},
    {0x54, &SVC::Wrap<&SVC::FlushProcessDataCache>, "FlushProcessDataCache"},
    {0x55, nullptr, "StartInterProcessDma"},
    {0x56, nullptr, "StopDma"},
    {0x57, nullptr, "GetDmaState"},
    {0x58, nullptr, "RestartDma"},
    {0x59, nullptr, "SetGpuProt"},
    {0x5A, nullptr, "SetWifiEnabled"},
    {0x5B, nullptr, "Unknown"},
    {0x5C, nullptr, "Unknown"},
    {0x5D, nullptr, "Unknown"},
    {0x5E, nullptr, "Unknown"},
    {0x5F, nullptr, "Unknown"},
    {0x60, nullptr, "DebugActiveProcess"},
    {0x61, nullptr, "BreakDebugProcess"},
    {0x62, nullptr, "TerminateDebugProcess"},
    {0x63, nullptr, "GetProcessDebugEvent"},
    {0x64, nullptr, "ContinueDebugEvent"},
    {0x65, &SVC::Wrap<&SVC::GetProcessList>, "GetProcessList"},
    {0x66, nullptr, "GetThreadList"},
    {0x67, nullptr, "GetDebugThreadContext"},
    {0x68, nullptr, "SetDebugThreadContext"},
    {0x69, nullptr, "QueryDebugProcessMemory"},
    {0x6A, nullptr, "ReadProcessMemory"},
    {0x6B, nullptr, "WriteProcessMemory"},
    {0x6C, nullptr, "SetHardwareBreakPoint"},
    {0x6D, nullptr, "GetDebugThreadParam"},
    {0x6E, nullptr, "Unknown"},
    {0x6F, nullptr, "Unknown"},
    {0x70, nullptr, "ControlProcessMemory"},
    {0x71, nullptr, "MapProcessMemory"},
    {0x72, nullptr, "UnmapProcessMemory"},
    {0x73, nullptr, "CreateCodeSet"},
    {0x74, nullptr, "RandomStub"},
    {0x75, nullptr, "CreateProcess"},
    {0x76, &SVC::Wrap<&SVC::TerminateProcess>, "TerminateProcess"},
    {0x77, nullptr, "SetProcessResourceLimits"},
    {0x78, nullptr, "CreateResourceLimit"},
    {0x79, &SVC::Wrap<&SVC::SetResourceLimitLimitValues>, "SetResourceLimitLimitValues"},
    {0x7A, nullptr, "AddCodeSegment"},
    {0x7B, nullptr, "Backdoor"},
    {0x7C, &SVC::Wrap<&SVC::KernelSetState>, "KernelSetState"},
    {0x7D, &SVC::Wrap<&SVC::QueryProcessMemory>, "QueryProcessMemory"},
    // Custom SVCs
    {0x7E, nullptr, "Unused"},
    {0x7F, nullptr, "Unused"},
    {0x80, nullptr, "CustomBackdoor"},
    {0x81, nullptr, "Unused"},
    {0x82, nullptr, "Unused"},
    {0x83, nullptr, "Unused"},
    {0x84, nullptr, "Unused"},
    {0x85, nullptr, "Unused"},
    {0x86, nullptr, "Unused"},
    {0x87, nullptr, "Unused"},
    {0x88, nullptr, "Unused"},
    {0x89, nullptr, "Unused"},
    {0x8A, nullptr, "Unused"},
    {0x8B, nullptr, "Unused"},
    {0x8C, nullptr, "Unused"},
    {0x8D, nullptr, "Unused"},
    {0x8E, nullptr, "Unused"},
    {0x8F, nullptr, "Unused"},
    {0x90, &SVC::Wrap<&SVC::ConvertVaToPa>, "ConvertVaToPa"},
    {0x91, nullptr, "FlushDataCacheRange"},
    {0x92, nullptr, "FlushEntireDataCache"},
    {0x93, &SVC::Wrap<&SVC::InvalidateInstructionCacheRange>, "InvalidateInstructionCacheRange"},
    {0x94, &SVC::Wrap<&SVC::InvalidateEntireInstructionCache>, "InvalidateEntireInstructionCache"},
    {0x95, nullptr, "Unused"},
    {0x96, nullptr, "Unused"},
    {0x97, nullptr, "Unused"},
    {0x98, nullptr, "Unused"},
    {0x99, nullptr, "Unused"},
    {0x9A, nullptr, "Unused"},
    {0x9B, nullptr, "Unused"},
    {0x9C, nullptr, "Unused"},
    {0x9D, nullptr, "Unused"},
    {0x9E, nullptr, "Unused"},
    {0x9F, nullptr, "Unused"},
    {0xA0, &SVC::Wrap<&SVC::MapProcessMemoryEx>, "MapProcessMemoryEx"},
    {0xA1, &SVC::Wrap<&SVC::UnmapProcessMemoryEx>, "UnmapProcessMemoryEx"},
    {0xA2, nullptr, "ControlMemoryEx"},
    {0xA3, nullptr, "ControlMemoryUnsafe"},
    {0xA4, nullptr, "Unused"},
    {0xA5, nullptr, "Unused"},
    {0xA6, nullptr, "Unused"},
    {0xA7, nullptr, "Unused"},
    {0xA8, nullptr, "Unused"},
    {0xA9, nullptr, "Unused"},
    {0xAA, nullptr, "Unused"},
    {0xAB, nullptr, "Unused"},
    {0xAC, nullptr, "Unused"},
    {0xAD, nullptr, "Unused"},
    {0xAE, nullptr, "Unused"},
    {0xAF, nullptr, "Unused"},
    {0xB0, nullptr, "ControlService"},
    {0xB1, nullptr, "CopyHandle"},
    {0xB2, nullptr, "TranslateHandle"},
    {0xB3, &SVC::Wrap<&SVC::ControlProcess>, "ControlProcess"},
}};

const SVC::FunctionDef* SVC::GetSVCInfo(u32 func_num) {
    if (func_num >= SVC_Table.size()) {
        LOG_ERROR(Kernel_SVC, "unknown svc=0x{:02X}", func_num);
        return nullptr;
    }
    return &SVC_Table[func_num];
}

void SVC::CallSVC(u32 immediate) {
    CITRA_PROFILE("Kernel", "SVC");

    // Lock the kernel mutex when we enter the kernel HLE.
    std::scoped_lock lock{kernel.GetHLELock()};

    DEBUG_ASSERT_MSG(kernel.GetCurrentProcess()->status == ProcessStatus::Running,
                     "Running threads from exiting processes is unimplemented");

    const FunctionDef* info = GetSVCInfo(immediate);
    LOG_TRACE(Kernel_SVC, "calling {}", info->name);
    if (info) {
        if (info->func) {
            (this->*(info->func))();
        } else {
            LOG_ERROR(Kernel_SVC, "unimplemented SVC function {}(..)", info->name);
        }
    }
}

SVC::SVC(Core::System& system) : system(system), kernel(system.Kernel()), memory(system.Memory()) {}

u32 SVC::GetReg(std::size_t n) {
    return system.GetRunningCore().GetReg(static_cast<int>(n));
}

void SVC::SetReg(std::size_t n, u32 value) {
    system.GetRunningCore().SetReg(static_cast<int>(n), value);
}

SVCContext::SVCContext(Core::System& system) : impl(std::make_unique<SVC>(system)) {}
SVCContext::~SVCContext() = default;

void SVCContext::CallSVC(u32 immediate) {
    impl->CallSVC(immediate);
}

} // namespace Kernel

SERIALIZE_EXPORT_IMPL(Kernel::SVC_SyncCallback)
SERIALIZE_EXPORT_IMPL(Kernel::SVC_IPCCallback)
