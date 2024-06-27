// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <boost/container/flat_set.hpp>
#include <boost/serialization/export.hpp>
#include "common/common_types.h"
#include "common/thread_queue_list.h"
#include "core/arm/arm_interface.h"
#include "core/hle/kernel/k_synchronization_object.h"
#include "core/hle/kernel/slab_helpers.h"
#include "core/hle/result.h"

namespace Kernel {

class KMutex;
class Process;

enum ThreadPriority : u32 {
    ThreadPrioHighest = 0,      ///< Highest thread priority
    ThreadPrioUserlandMax = 24, ///< Highest thread priority for userland apps
    ThreadPrioDefault = 48,     ///< Default thread priority for userland apps
    ThreadPrioLowest = 63,      ///< Lowest thread priority
};

enum ThreadProcessorId : s32 {
    ThreadProcessorIdDefault = -2, ///< Run thread on default core specified by exheader
    ThreadProcessorIdAll = -1,     ///< Run thread on either core
    ThreadProcessorId0 = 0,        ///< Run thread on core 0 (AppCore)
    ThreadProcessorId1 = 1,        ///< Run thread on core 1 (SysCore)
    ThreadProcessorId2 = 2,        ///< Run thread on core 2 (additional n3ds core)
    ThreadProcessorId3 = 3,        ///< Run thread on core 3 (additional n3ds core)
    ThreadProcessorIdMax = 4,      ///< Processor ID must be less than this
};

enum class ThreadStatus {
    Running,      ///< Currently running
    Ready,        ///< Ready to run
    WaitArb,      ///< Waiting on an address arbiter
    WaitSleep,    ///< Waiting due to a SleepThread SVC
    WaitIPC,      ///< Waiting for the reply from an IPC request
    WaitSynchAny, ///< Waiting due to WaitSynch1 or WaitSynchN with wait_all = false
    WaitSynchAll, ///< Waiting due to WaitSynchronizationN with wait_all = true
    WaitHleEvent, ///< Waiting due to an HLE handler pausing the thread
    Dormant,      ///< Created but not yet made ready
    Dead          ///< Run to completion, or forcefully terminated
};

enum class ThreadWakeupReason : u32 {
    Signal, // The thread was woken up by WakeupAllWaitingThreads due to an object signal.
    Timeout // The thread was woken up due to a wait timeout.
};

class KThread;

class WakeupCallback {
public:
    virtual ~WakeupCallback() = default;
    virtual void WakeUp(ThreadWakeupReason reason, KThread* thread,
                        KSynchronizationObject* object) = 0;

private:
    template <class Archive>
    void serialize(Archive& ar, const unsigned int);
    friend class boost::serialization::access;
};

class ThreadManager {
public:
    explicit ThreadManager(Kernel::KernelSystem& kernel, u32 core_id);
    ~ThreadManager();

    KThread* GetCurrentThread() const {
        return current_thread;
    }

    /**
     * Reschedules to the next available thread (call after current thread is suspended)
     */
    void Reschedule();

    /**
     * Returns whether there are any threads that are ready to run.
     */
    bool HaveReadyThreads();

    /**
     * Waits the current thread on a sleep
     */
    void WaitCurrentThread_Sleep();

    /**
     * Stops the current thread and removes it from the thread_list
     */
    void ExitCurrentThread();

    /**
     * Terminates all threads belonging to a specific process.
     */
    void TerminateProcessThreads(Process* process);

    /**
     * Get a const reference to the thread list for debug use
     */
    std::vector<KThread*>& GetThreadList() {
        return thread_list;
    }

    void SetCPU(Core::ARM_Interface& cpu_) {
        cpu = &cpu_;
    }

private:
    /**
     * Switches the CPU's active thread context to that of the specified thread
     * @param new_thread The thread to switch to
     */
    void SwitchContext(KThread* new_thread);

    /**
     * Pops and returns the next thread from the thread queue
     * @return A pointer to the next ready thread
     */
    KThread* PopNextReadyThread();

    /**
     * Callback that will wake up the thread it was scheduled for
     * @param thread_id The ID of the thread that's been awoken
     * @param cycles_late The number of CPU cycles that have passed since the desired wakeup time
     */
    void ThreadWakeupCallback(u64 thread_id, s64 cycles_late);

    /// Boost low priority starved threads
    void PriorityBoostStarvedThreads();

private:
    /// Boost low priority starved threads
    void PriorityBoostStarvedThreads();

    Kernel::KernelSystem& kernel;
    Core::ARM_Interface* cpu{};
    KThread* current_thread{};
    Common::ThreadQueueList<KThread*, ThreadPrioLowest + 1> ready_queue;
    std::deque<KThread*> unscheduled_ready_queue;
    std::unordered_map<u64, KThread*> wakeup_callback_table;
    Core::TimingEventType* thread_wakeup_event_type{};
    std::vector<KThread*> thread_list;

    friend class KThread;
    friend class KernelSystem;

    friend class boost::serialization::access;
    template <class Archive>
    void serialize(Archive& ar, const unsigned int);
};

class KThread final : public KAutoObjectWithSlabHeapAndContainer<KThread, KSynchronizationObject> {
    KERNEL_AUTOOBJECT_TRAITS(KThread, KSynchronizationObject);

    using ThreadContext = Core::ARM_Interface::ThreadContext;

public:
    explicit KThread(KernelSystem&);
    ~KThread() override;

    Result Initialize(std::string name, VAddr entry_point, u32 priority, u32 arg, s32 processor_id,
                      VAddr stack_top, Process* owner);

    static void PostDestroy(uintptr_t arg);

    bool ShouldWait(const KThread* thread) const override {
        return m_status != ThreadStatus::Dead;
    }

    void Acquire(KThread* thread) override {
        ASSERT_MSG(!ShouldWait(thread), "object unavailable!");
    }

    u32 GetThreadId() const {
        return m_thread_id;
    }

    u32 GetCurrentPriority() const {
        return m_current_priority;
    }

    VAddr GetWaitAddress() const {
        return m_wait_address;
    }

    VAddr GetTLSAddress() const {
        return m_tls_address;
    }

    ThreadContext& GetContext() {
        return m_context;
    }

    const ThreadContext& GetContext() const {
        return m_context;
    }

    ThreadStatus GetStatus() const {
        return m_status;
    }

    Process* GetOwner() const override {
        return m_owner;
    }

    void SetWakeupCallback(std::shared_ptr<WakeupCallback>&& callback) {
        m_wakeup_callback = callback;
    }

    u32 GetPriority() const {
        return m_current_priority;
    }

    void SetPriority(u32 priority);

    /**
     * Boost's a thread's priority to the best priority among the thread's held mutexes.
     * This prevents priority inversion via priority inheritance.
     */
    void UpdatePriority();

    /**
     * Temporarily boosts the thread's priority until the next time it is scheduled
     * @param priority The new priority
     */
    void BoostPriority(u32 priority);

    /**
     * Resumes a thread from waiting
     */
    void ResumeFromWait();

    /**
     * Schedules an event to wake up the specified thread after the specified delay
     * @param nanoseconds The time this thread will be allowed to sleep for
     * @param thread_safe_mode Set to true if called from a different thread than the emulator
     * thread, such as coroutines.
     */
    void WakeAfterDelay(s64 nanoseconds, bool thread_safe_mode = false);

    /**
     * Sets the result after the thread awakens (from either WaitSynchronization SVC)
     * @param result Value to set to the returned result
     */
    void SetWaitSynchronizationResult(Result result);

    /**
     * Sets the output parameter value after the thread awakens (from WaitSynchronizationN SVC only)
     * @param output Value to set to the output parameter
     */
    void SetWaitSynchronizationOutput(s32 output);

    /**
     * Retrieves the index that this particular object occupies in the list of objects
     * that the thread passed to WaitSynchronizationN, starting the search from the last element.
     * It is used to set the output value of WaitSynchronizationN when the thread is awakened.
     * When a thread wakes up due to an object signal, the kernel will use the index of the last
     * matching object in the wait objects list in case of having multiple instances of the same
     * object in the list.
     * @param object Object to query the index of.
     */
    s32 GetWaitObjectIndex(const KSynchronizationObject* object) const;

    /**
     * Stops a thread, invalidating it from further use
     */
    void Stop();

    VAddr GetCommandBufferAddress() const;

    /**
     * Returns whether this thread is waiting for all the objects in
     * its wait list to become ready, as a result of a WaitSynchronizationN call
     * with wait_all = true.
     */
    bool IsSleepingOnWaitAll() const {
        return m_status == ThreadStatus::WaitSynchAll;
    }

public:
    ThreadManager* m_manager{};
    ThreadContext m_context{};
    u32 m_thread_id;
    u32 m_core_id;
    bool m_can_schedule{true};
    ThreadStatus m_status;
    VAddr m_entry_point;
    VAddr m_stack_top;
    u32 m_nominal_priority;
    u32 m_current_priority;
    u64 m_last_running_ticks;
    s32 m_processor_id;
    VAddr m_tls_address;
    boost::container::flat_set<KMutex*> m_held_mutexes;
    boost::container::flat_set<KMutex*> m_pending_mutexes;
    Process* m_owner{};
    std::vector<KSynchronizationObject*> m_wait_objects;
    VAddr m_wait_address;
    std::string m_name{};
    std::shared_ptr<WakeupCallback> m_wakeup_callback{};

    friend class boost::serialization::access;
    template <class Archive>
    void serialize(Archive& ar, const u32 file_version);
};

} // namespace Kernel

BOOST_CLASS_EXPORT_KEY(Kernel::KThread)
CONSTRUCT_KERNEL_OBJECT(Kernel::KThread)
