// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/archives.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_resource_limit.h"
#include "core/hle/kernel/k_timer.h"

SERIALIZE_EXPORT_IMPL(Kernel::KTimer)

namespace Kernel {

KTimer::KTimer(KernelSystem& kernel)
    : KAutoObjectWithSlabHeapAndContainer(kernel), m_timer_manager(kernel.GetTimerManager()) {}

KTimer::~KTimer() = default;

void KTimer::Initialize(Process* owner, ResetType reset_type) {
    // Open a reference to the owner process.
    owner->Open();

    // Set member variables.
    m_owner = owner;
    m_reset_type = reset_type;

    // Register to TimerManager
    m_callback_id = m_timer_manager.GetNextCallbackId();
    m_timer_manager.Register(m_callback_id, this);
}

void KTimer::Finalize() {
    this->Cancel();
    m_timer_manager.Unregister(m_callback_id);
}

void KTimer::PostDestroy(uintptr_t arg) {
    // Release the session count resource the owner process holds.
    Process* owner = reinterpret_cast<Process*>(arg);
    owner->ReleaseResource(ResourceLimitType::Timer, 1);
    owner->Close();
}

bool KTimer::ShouldWait(const KThread* thread) const {
    return !m_signaled;
}

void KTimer::Acquire(KThread* thread) {
    ASSERT_MSG(!ShouldWait(thread), "object unavailable!");
    if (m_reset_type == ResetType::OneShot) {
        m_signaled = false;
    }
}

void KTimer::Set(s64 initial, s64 interval) {
    // Ensure we get rid of any previous scheduled event
    this->Cancel();

    // Set member variables
    m_initial_delay = initial;
    m_interval_delay = interval;

    if (initial == 0) {
        // Immediately invoke the callback
        this->Signal(0);
    } else {
        auto& timing = m_kernel.timing;
        timing.ScheduleEvent(nsToCycles(initial), m_timer_manager.GetEventType(), m_callback_id);
    }
}

void KTimer::Cancel() {
    auto& timing = m_kernel.timing;
    timing.UnscheduleEvent(m_timer_manager.GetEventType(), m_callback_id);
}

void KTimer::Clear() {
    m_signaled = false;
}

void KTimer::WakeupAllWaitingThreads() {
    KSynchronizationObject::WakeupAllWaitingThreads();
    if (m_reset_type == ResetType::Pulse) {
        m_signaled = false;
    }
}

void KTimer::Signal(s64 cycles_late) {
    LOG_TRACE(Kernel, "Timer {} fired", GetObjectId());
    m_signaled = true;

    // Resume all waiting threads
    this->WakeupAllWaitingThreads();

    // Reschedule the timer with the interval delay
    if (m_interval_delay != 0) {
        auto& timing = m_kernel.timing;
        const s64 cycles_into_future = nsToCycles(m_interval_delay) - cycles_late;
        timing.ScheduleEvent(cycles_into_future, m_timer_manager.GetEventType(), m_callback_id);
    }
}

void TimerManager::TimerCallback(u64 callback_id, s64 cycles_late) {
    KTimer* timer = m_timer_callback_table.at(callback_id);
    ASSERT_MSG(timer, "Callback fired for invalid timer {:016x}", callback_id);
    timer->Signal(cycles_late);
}

TimerManager::TimerManager(Core::Timing& timing) : m_timing(timing) {
    m_timer_callback_event_type =
        timing.RegisterEvent("TimerCallback", [this](u64 thread_id, s64 cycle_late) {
            this->TimerCallback(thread_id, cycle_late);
        });
}

TimerManager::~TimerManager() = default;

} // namespace Kernel
