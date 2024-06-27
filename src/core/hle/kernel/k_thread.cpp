// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <boost/serialization/string.hpp>
#include <boost/serialization/unordered_map.hpp>
#include <boost/serialization/vector.hpp>
#include <boost/serialization/weak_ptr.hpp>
#include "common/archives.h"
#include "common/assert.h"
#include "common/common_types.h"
#include "common/logging/log.h"
#include "common/serialization/boost_flat_set.h"
#include "core/arm/arm_interface.h"
#include "core/arm/skyeye_common/armstate.h"
#include "core/core_timing.h"
#include "core/hle/kernel/errors.h"
#include "core/hle/kernel/k_mutex.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_resource_limit.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/result.h"
#include "core/memory.h"

SERIALIZE_EXPORT_IMPL(Kernel::KThread)
SERIALIZE_EXPORT_IMPL(Kernel::WakeupCallback)

namespace Kernel {

template <class Archive>
void ThreadManager::serialize(Archive& ar, const unsigned int) {
    ar& current_thread;
    ar& ready_queue;
    ar& wakeup_callback_table;
    ar& thread_list;
}
SERIALIZE_IMPL(ThreadManager)

template <class Archive>
void WakeupCallback::serialize(Archive& ar, const unsigned int) {}
SERIALIZE_IMPL(WakeupCallback)

ThreadManager::ThreadManager(Kernel::KernelSystem& kernel, u32 core_id) : kernel(kernel) {
    thread_wakeup_event_type = kernel.timing.RegisterEvent(
        "ThreadWakeupCallback_" + std::to_string(core_id),
        [this](u64 thread_id, s64 cycle_late) { ThreadWakeupCallback(thread_id, cycle_late); });
}

ThreadManager::~ThreadManager() {
    for (auto& t : thread_list) {
        t->Stop();
    }
}

void ThreadManager::SwitchContext(KThread* new_thread) {
    auto& timing = kernel.timing;
    KThread* previous_thread = GetCurrentThread();
    Process* previous_process = nullptr;

    // Save context for previous thread
    if (previous_thread) {
        previous_process = previous_thread->GetOwner();
        previous_thread->m_last_running_ticks = cpu->GetTimer().GetTicks();
        cpu->SaveContext(previous_thread->m_context);

        if (previous_thread->m_status == ThreadStatus::Running) {
            // This is only the case when a reschedule is triggered without the current thread
            // yielding execution (i.e. an event triggered, system core time-sliced, etc)
            ready_queue.push_front(previous_thread->m_current_priority, previous_thread);
            previous_thread->m_status = ThreadStatus::Ready;
        }
    }

    // Load context of new thread
    if (new_thread) {
        ASSERT_MSG(new_thread->m_status == ThreadStatus::Ready,
                   "Thread must be ready to become running.");

        // Cancel any outstanding wakeup events for this thread
        timing.UnscheduleEvent(thread_wakeup_event_type, new_thread->m_thread_id);

        current_thread = new_thread;

        ready_queue.remove(new_thread->m_current_priority, new_thread);
        new_thread->m_status = ThreadStatus::Running;

        ASSERT(current_thread->GetOwner());
        if (previous_process != current_thread->GetOwner()) {
            kernel.SetCurrentProcessForCPU(current_thread->GetOwner(), cpu->GetID());
        }

        // Restores thread to its nominal priority if it has been temporarily changed
        new_thread->m_current_priority = new_thread->m_nominal_priority;

        cpu->LoadContext(new_thread->m_context);
        cpu->SetCP15Register(CP15_THREAD_URO, new_thread->GetTLSAddress());
    } else {
        current_thread = nullptr;
        // Note: We do not reset the current process and current page table when idling because
        // technically we haven't changed processes, our threads are just paused.
    }
}

KThread* ThreadManager::PopNextReadyThread() {
    KThread* next = nullptr;
    KThread* thread = GetCurrentThread();

    if (thread && thread->m_status == ThreadStatus::Running) {
        do {
            // We have to do better than the current thread.
            // This call returns null when that's not possible.
            next = ready_queue.pop_first_better(thread->m_current_priority);
            if (!next) {
                // Otherwise just keep going with the current thread
                next = thread;
                break;
            } else if (!next->m_can_schedule)
                unscheduled_ready_queue.push_back(next);
        } while (!next->m_can_schedule);
    } else {
        do {
            next = ready_queue.pop_first();
            if (next && !next->m_can_schedule)
                unscheduled_ready_queue.push_back(next);
        } while (next && !next->m_can_schedule);
    }

    while (!unscheduled_ready_queue.empty()) {
        auto t = std::move(unscheduled_ready_queue.back());
        ready_queue.push_back(t->m_current_priority, t);
        unscheduled_ready_queue.pop_back();
    }

    return next;
}

void ThreadManager::WaitCurrentThread_Sleep() {
    KThread* thread = GetCurrentThread();
    thread->m_status = ThreadStatus::WaitSleep;
}

void ThreadManager::ExitCurrentThread() {
    current_thread->Stop();
    std::erase(thread_list, current_thread);
    kernel.PrepareReschedule();
}

void ThreadManager::TerminateProcessThreads(Process* process) {
    auto iter = thread_list.begin();
    while (iter != thread_list.end()) {
        auto& thread = *iter;
        if (thread == current_thread || thread->GetOwner() != process) {
            iter++;
            continue;
        }

        if (thread->m_status != ThreadStatus::WaitSynchAny &&
            thread->m_status != ThreadStatus::WaitSynchAll) {
            // TODO: How does the real kernel handle non-waiting threads?
            LOG_WARNING(Kernel, "Terminating non-waiting thread {}", thread->m_thread_id);
        }

        thread->Stop();
        iter = thread_list.erase(iter);
    }

    // Kill the current thread last, if applicable.
    if (current_thread != nullptr && current_thread->GetOwner() == process) {
        ExitCurrentThread();
    }
}

void ThreadManager::ThreadWakeupCallback(u64 thread_id, s64 cycles_late) {
    KThread* thread = wakeup_callback_table.at(thread_id);
    if (thread == nullptr) {
        LOG_CRITICAL(Kernel, "Callback fired for invalid thread {:08X}", thread_id);
        return;
    }

    if (thread->m_status == ThreadStatus::WaitSynchAny ||
        thread->m_status == ThreadStatus::WaitSynchAll ||
        thread->m_status == ThreadStatus::WaitArb ||
        thread->m_status == ThreadStatus::WaitHleEvent) {

        // Invoke the wakeup callback before clearing the wait objects
        if (thread->m_wakeup_callback) {
            thread->m_wakeup_callback->WakeUp(ThreadWakeupReason::Timeout, thread, nullptr);
        }

        // Remove the thread from each of its waiting objects' waitlists
        for (KSynchronizationObject* object : thread->m_wait_objects) {
            object->RemoveWaitingThread(thread);
        }
        thread->m_wait_objects.clear();
    }

    thread->ResumeFromWait();
}

bool ThreadManager::HaveReadyThreads() {
    return ready_queue.get_first() != nullptr;
}

void ThreadManager::PriorityBoostStarvedThreads() {
    const u64 current_ticks = kernel.timing.GetTicks();

    for (auto& thread : thread_list) {
        const u64 boost_ticks = 1400000;

        u64 delta = current_ticks - thread->m_last_running_ticks;

        if (thread->m_status == ThreadStatus::Ready && delta > boost_ticks) {
            const s32 priority = std::max(ready_queue.get_first()->m_current_priority, 40u);
            thread->BoostPriority(priority);
        }
    }
}

void ThreadManager::Reschedule() {
    if (Settings::values.priority_boost) {
        PriorityBoostStarvedThreads();
    }

    KThread* cur = GetCurrentThread();
    KThread* next = PopNextReadyThread();

    if (cur && next) {
        LOG_TRACE(Kernel, "context switch {} -> {}", cur->GetObjectId(), next->GetObjectId());
    } else if (cur) {
        LOG_TRACE(Kernel, "context switch {} -> idle", cur->GetObjectId());
    } else if (next) {
        LOG_TRACE(Kernel, "context switch idle -> {}", next->GetObjectId());
    } else {
        LOG_TRACE(Kernel, "context switch idle -> idle, do nothing");
        return;
    }

    SwitchContext(next);
}

KThread::KThread(KernelSystem& kernel) : KAutoObjectWithSlabHeapAndContainer(kernel) {}

KThread::~KThread() = default;

void KThread::Stop() {
    // Cancel any outstanding wakeup events for this thread
    auto& timing = m_kernel.timing;
    timing.UnscheduleEvent(m_manager->thread_wakeup_event_type, m_thread_id);
    m_manager->wakeup_callback_table.erase(m_thread_id);

    // Clean up thread from ready queue
    // This is only needed when the thread is termintated forcefully (SVC TerminateProcess)
    if (m_status == ThreadStatus::Ready) {
        m_manager->ready_queue.remove(m_current_priority, this);
    }

    // Wake all threads waiting on this thread.
    m_status = ThreadStatus::Dead;
    this->WakeupAllWaitingThreads();

    // Clean up any dangling references in objects that this thread was waiting for
    for (KSynchronizationObject* object : m_wait_objects) {
        object->RemoveWaitingThread(this);
    }
    m_wait_objects.clear();

    // Release all the mutexes that this thread holds
    ReleaseThreadMutexes(this);

    // Mark the TLS slot in the thread's page as free.
    const u32 tls_page = (m_tls_address - Memory::TLS_AREA_VADDR) / Memory::CITRA_PAGE_SIZE;
    const u32 tls_slot = ((m_tls_address - Memory::TLS_AREA_VADDR) % Memory::CITRA_PAGE_SIZE) /
                         Memory::TLS_ENTRY_SIZE;
    m_owner->tls_slots[tls_page].reset(tls_slot);
}

void KThread::WakeAfterDelay(s64 nanoseconds, bool thread_safe_mode) {
    // Don't schedule a wakeup if the thread wants to wait forever
    if (nanoseconds == -1) {
        return;
    }
    if ((nanoseconds & 0x7000000000000000) == 0x7000000000000000) {
        // fx jit bug
        nanoseconds &= 0xFFFFFFFF;
    }
    auto& timing = m_kernel.timing;
    const size_t core = thread_safe_mode ? m_core_id : std::numeric_limits<std::size_t>::max();
    timing.ScheduleEvent(nsToCycles(nanoseconds), m_manager->thread_wakeup_event_type, m_thread_id,
                         core, thread_safe_mode);
}

void KThread::ResumeFromWait() {
    ASSERT_MSG(m_wait_objects.empty(), "Thread is waking up while waiting for objects");

    switch (m_status) {
    case ThreadStatus::WaitSynchAll:
    case ThreadStatus::WaitSynchAny:
    case ThreadStatus::WaitHleEvent:
    case ThreadStatus::WaitArb:
    case ThreadStatus::WaitSleep:
    case ThreadStatus::WaitIPC:
    case ThreadStatus::Dormant:
        break;

    case ThreadStatus::Ready:
        // The thread's wakeup callback must have already been cleared when the thread was first
        // awoken.
        ASSERT(m_wakeup_callback == nullptr);
        // If the thread is waiting on multiple wait objects, it might be awoken more than once
        // before actually resuming. We can ignore subsequent wakeups if the thread status has
        // already been set to ThreadStatus::Ready.
        return;

    case ThreadStatus::Running:
        DEBUG_ASSERT_MSG(false, "Thread with object id {} has already resumed.", GetObjectId());
        return;
    case ThreadStatus::Dead:
        // This should never happen, as threads must complete before being stopped.
        DEBUG_ASSERT_MSG(false, "Thread with object id {} cannot be resumed because it's DEAD.",
                         GetObjectId());
        return;
    }

    // Mark as ready and reschedule.
    m_wakeup_callback = nullptr;
    m_manager->ready_queue.push_back(m_current_priority, this);
    m_status = ThreadStatus::Ready;
    m_kernel.PrepareReschedule();
}

/**
 * Resets a thread context, making it ready to be scheduled and run by the CPU
 * @param context Thread context to reset
 * @param stack_top Address of the top of the stack
 * @param entry_point Address of entry point for execution
 * @param arg User argument for thread
 */
static void ResetThreadContext(Core::ARM_Interface::ThreadContext& context, u32 stack_top,
                               u32 entry_point, u32 arg) {
    context.cpu_registers[0] = arg;
    context.SetProgramCounter(entry_point);
    context.SetStackPointer(stack_top);
    context.cpsr = USER32MODE | ((entry_point & 1) << 5); // Usermode and THUMB mode
    context.fpscr = FPSCR_DEFAULT_NAN | FPSCR_FLUSH_TO_ZERO | FPSCR_ROUND_TOZERO | FPSCR_IXC;
}

Result KThread::Initialize(std::string name, VAddr entry_point, u32 priority, u32 arg,
                           s32 processor_id, VAddr stack_top, Process* owner_process) {
    R_UNLESS(priority <= ThreadPrioLowest, ResultOutOfRange);
    R_UNLESS(processor_id <= ThreadProcessorIdMax, ResultOutOfRangeKernel);

    // Open a reference to our owner process
    m_owner = owner_process;
    m_owner->Open();

    // Set last running ticks.
    auto& timing = m_kernel.timing;
    m_last_running_ticks = timing.GetTimer(processor_id)->GetTicks();

    // Set member variables.
    m_thread_id = m_kernel.NewThreadId();
    m_status = ThreadStatus::Ready;
    m_entry_point = entry_point;
    m_stack_top = stack_top;
    m_nominal_priority = m_current_priority = priority;
    m_processor_id = processor_id;
    m_wait_objects.clear();
    m_wait_address = 0;
    m_name = std::move(name);

    // Register thread in the thread manager.
    auto& thread_manager = m_kernel.GetThreadManager(processor_id);
    m_manager = std::addressof(thread_manager);
    m_manager->thread_list.push_back(this);
    m_manager->ready_queue.prepare(priority);
    m_manager->wakeup_callback_table[m_thread_id] = this;

    // Allocate the thread local region.
    R_TRY(m_owner->AllocateThreadLocalStorage(std::addressof(m_tls_address)));

    // Reset the thread context.
    ResetThreadContext(m_context, stack_top, entry_point, arg);

    // Mark thread as ready and return
    m_manager->ready_queue.push_back(m_current_priority, this);
    return ResultSuccess;
}

void KThread::SetPriority(u32 priority) {
    ASSERT_MSG(priority <= ThreadPrioLowest && priority >= ThreadPrioHighest,
               "Invalid priority value.");

    // If thread was ready, adjust queues
    if (m_status == ThreadStatus::Ready) {
        m_manager->ready_queue.move(this, m_current_priority, priority);
    } else {
        m_manager->ready_queue.prepare(priority);
    }

    // Set the priority
    m_nominal_priority = m_current_priority = priority;
}

void KThread::UpdatePriority() {
    u32 best_priority = m_nominal_priority;
    for (KMutex* mutex : m_held_mutexes) {
        if (mutex->GetPriority() < best_priority) {
            best_priority = mutex->GetPriority();
        }
    }
    this->BoostPriority(best_priority);
}

void KThread::BoostPriority(u32 priority) {
    // If thread was ready, adjust queues
    if (m_status == ThreadStatus::Ready) {
        m_manager->ready_queue.move(this, m_current_priority, priority);
    } else {
        m_manager->ready_queue.prepare(priority);
    }
    m_current_priority = priority;
}

void KThread::PostDestroy(uintptr_t arg) {
    Process* owner = reinterpret_cast<Process*>(arg);
    if (owner != nullptr) {
        owner->ReleaseResource(ResourceLimitType::Thread, 1);
        owner->Close();
    }
}

void KThread::SetWaitSynchronizationResult(Result result) {
    m_context.cpu_registers[0] = result.raw;
}

void KThread::SetWaitSynchronizationOutput(s32 output) {
    m_context.cpu_registers[1] = output;
}

s32 KThread::GetWaitObjectIndex(const KSynchronizationObject* object) const {
    ASSERT_MSG(!m_wait_objects.empty(), "Thread is not waiting for anything");
    const auto match = std::find(m_wait_objects.rbegin(), m_wait_objects.rend(), object);
    return static_cast<s32>(std::distance(match, m_wait_objects.rend()) - 1);
}

VAddr KThread::GetCommandBufferAddress() const {
    // Offset from the start of TLS at which the IPC command buffer begins.
    constexpr u32 command_header_offset = 0x80;
    return GetTLSAddress() + command_header_offset;
}

template <class Archive>
void KThread::serialize(Archive& ar, const unsigned int file_version) {
    ar& boost::serialization::base_object<KSynchronizationObject>(*this);
    ar& m_context;
    ar& m_thread_id;
    ar& m_status;
    ar& m_entry_point;
    ar& m_stack_top;
    ar& m_nominal_priority;
    ar& m_current_priority;
    ar& m_last_running_ticks;
    ar& m_processor_id;
    ar& m_tls_address;
    ar& m_held_mutexes;
    ar& m_pending_mutexes;
    ar& m_owner;
    ar& m_wait_objects;
    ar& m_wait_address;
    ar& m_name;
    ar& m_wakeup_callback;
}

SERIALIZE_IMPL(KThread)

} // namespace Kernel
