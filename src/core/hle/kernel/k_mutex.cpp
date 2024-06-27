// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <boost/serialization/base_object.hpp>
#include "common/archives.h"
#include "common/assert.h"
#include "core/hle/kernel/errors.h"
#include "core/hle/kernel/k_mutex.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_resource_limit.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"

SERIALIZE_EXPORT_IMPL(Kernel::KMutex)

namespace Kernel {

void ReleaseThreadMutexes(KThread* thread) {
    for (KMutex* mtx : thread->m_held_mutexes) {
        mtx->m_lock_count = 0;
        mtx->m_holding_thread = nullptr;
        mtx->WakeupAllWaitingThreads();
    }
    thread->m_held_mutexes.clear();
}

KMutex::KMutex(KernelSystem& kernel) : KAutoObjectWithSlabHeapAndContainer(kernel) {}

KMutex::~KMutex() = default;

void KMutex::Initialize(Process* owner, bool initial_locked) {
    // Open a reference to the owner process.
    if (owner) {
        owner->Open();
        m_owner = owner;
    }

    // Set default priority
    m_priority = ThreadPrioLowest;

    // Acquire mutex with current thread if initialized as locked
    if (initial_locked) {
        KThread* thread = m_kernel.GetCurrentThreadManager().GetCurrentThread();
        this->Acquire(thread);
    }
}

void KMutex::PostDestroy(uintptr_t arg) {
    Process* owner = reinterpret_cast<Process*>(arg);
    if (owner != nullptr) {
        owner->ReleaseResource(ResourceLimitType::Mutex, 1);
        owner->Close();
    }
}

bool KMutex::ShouldWait(const KThread* thread) const {
    return m_lock_count > 0 && thread != m_holding_thread;
}

void KMutex::Acquire(KThread* thread) {
    ASSERT_MSG(!ShouldWait(thread), "object unavailable!");

    // Actually "acquire" the mutex only if we don't already have it
    if (m_lock_count == 0) {
        m_priority = thread->m_current_priority;
        thread->m_held_mutexes.insert(this);
        m_holding_thread = thread;
        thread->UpdatePriority();
        m_kernel.PrepareReschedule();
    }

    m_lock_count++;
}

Result KMutex::Release(KThread* thread) {
    // We can only release the mutex if it's held by the calling thread.
    if (thread != m_holding_thread) {
        if (m_holding_thread) {
            LOG_ERROR(
                Kernel,
                "Tried to release a mutex (owned by thread id {}) from a different thread id {}",
                m_holding_thread->m_thread_id, thread->m_thread_id);
        }
        return Result(ErrCodes::WrongLockingThread, ErrorModule::Kernel,
                      ErrorSummary::InvalidArgument, ErrorLevel::Permanent);
    }

    // Note: It should not be possible for the situation where the mutex has a holding thread with a
    // zero lock count to occur. The real kernel still checks for this, so we do too.
    if (m_lock_count <= 0) {
        return Result(ErrorDescription::InvalidResultValue, ErrorModule::Kernel,
                      ErrorSummary::InvalidState, ErrorLevel::Permanent);
    }

    m_lock_count--;

    // Yield to the next thread only if we've fully released the mutex
    if (m_lock_count == 0) {
        m_holding_thread->m_held_mutexes.erase(this);
        m_holding_thread->UpdatePriority();
        m_holding_thread = nullptr;
        WakeupAllWaitingThreads();
        m_kernel.PrepareReschedule();
    }

    return ResultSuccess;
}

void KMutex::AddWaitingThread(KThread* thread) {
    KSynchronizationObject::AddWaitingThread(thread);
    thread->m_pending_mutexes.insert(this);
    this->UpdatePriority();
}

void KMutex::RemoveWaitingThread(KThread* thread) {
    KSynchronizationObject::RemoveWaitingThread(thread);
    thread->m_pending_mutexes.erase(this);
    this->UpdatePriority();
}

void KMutex::UpdatePriority() {
    if (!m_holding_thread) {
        return;
    }

    u32 best_priority = ThreadPrioLowest;
    for (const KThread* waiter : GetWaitingThreads()) {
        if (waiter->m_current_priority < best_priority) {
            best_priority = waiter->m_current_priority;
        }
    }

    if (best_priority != m_priority) {
        m_priority = best_priority;
        m_holding_thread->UpdatePriority();
    }
}

template <class Archive>
void KMutex::serialize(Archive& ar, const u32 file_version) {
    ar& boost::serialization::base_object<KSynchronizationObject>(*this);
    ar& m_lock_count;
    ar& m_priority;
    ar& m_holding_thread;
}

SERIALIZE_IMPL(KMutex)

} // namespace Kernel
