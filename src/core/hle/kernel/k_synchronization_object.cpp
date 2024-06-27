// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <utility>
#include <boost/serialization/base_object.hpp>
#include <boost/serialization/shared_ptr.hpp>
#include <boost/serialization/vector.hpp>
#include "common/archives.h"
#include "common/assert.h"
#include "core/hle/kernel/k_synchronization_object.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"

namespace Kernel {

KSynchronizationObject::KSynchronizationObject(KernelSystem& kernel) : KAutoObject(kernel) {}

KSynchronizationObject::~KSynchronizationObject() = default;

void KSynchronizationObject::AddWaitingThread(KThread* thread) {
    auto it = std::ranges::find(waiting_threads, thread);
    if (it == waiting_threads.end()) {
        waiting_threads.push_back(thread);
    }
}

void KSynchronizationObject::RemoveWaitingThread(KThread* thread) {
    // If a thread passed multiple handles to the same object,
    // the kernel might attempt to remove the thread from the object's
    // waiting threads list multiple times.
    auto it = std::ranges::find(waiting_threads, thread);
    if (it != waiting_threads.end()) {
        waiting_threads.erase(it);
    }
}

KThread* KSynchronizationObject::GetHighestPriorityReadyThread() const {
    KThread* candidate = nullptr;
    u32 candidate_priority = ThreadPrioLowest + 1;

    for (auto* thread : waiting_threads) {
        // The list of waiting threads must not contain threads that are not waiting to be awakened.
        ASSERT_MSG(thread->GetStatus() == ThreadStatus::WaitSynchAny ||
                       thread->GetStatus() == ThreadStatus::WaitSynchAll ||
                       thread->GetStatus() == ThreadStatus::WaitHleEvent,
                   "Inconsistent thread statuses in waiting_threads");

        if (thread->GetCurrentPriority() >= candidate_priority || ShouldWait(thread)) {
            continue;
        }

        // A thread is ready to run if it's either in ThreadStatus::WaitSynchAny or
        // in ThreadStatus::WaitSynchAll and the rest of the objects it is waiting on are ready.
        bool ready_to_run = true;
        if (thread->GetStatus() == ThreadStatus::WaitSynchAll) {
            ready_to_run =
                std::ranges::none_of(thread->m_wait_objects, [thread](const auto* object) {
                    return object->ShouldWait(thread);
                });
        }

        if (ready_to_run) {
            candidate = thread;
            candidate_priority = thread->GetCurrentPriority();
        }
    }

    return candidate;
}

void KSynchronizationObject::WakeupAllWaitingThreads() {
    while (auto thread = GetHighestPriorityReadyThread()) {
        if (!thread->IsSleepingOnWaitAll()) {
            Acquire(thread);
        } else {
            for (auto& object : thread->m_wait_objects) {
                object->Acquire(thread);
            }
        }

        // Invoke the wakeup callback before clearing the wait objects
        if (thread->m_wakeup_callback) {
            thread->m_wakeup_callback->WakeUp(ThreadWakeupReason::Signal, thread, this);
        }

        for (auto& object : thread->m_wait_objects) {
            object->RemoveWaitingThread(thread);
        }
        thread->m_wait_objects.clear();
        thread->ResumeFromWait();
    }

    if (hle_notifier) {
        hle_notifier();
    }
}

const std::vector<KThread*>& KSynchronizationObject::GetWaitingThreads() const {
    return waiting_threads;
}

void KSynchronizationObject::SetHLENotifier(std::function<void()> callback) {
    hle_notifier = std::move(callback);
}

template <class Archive>
void KSynchronizationObject::serialize(Archive& ar, const unsigned int file_version) {
    ar& boost::serialization::base_object<KAutoObject>(*this);
    ar& waiting_threads;
    // NB: hle_notifier *not* serialized since it's a callback!
    // Fortunately it's only used in one place (DSP) so we can reconstruct it there
}
SERIALIZE_IMPL(KSynchronizationObject)

} // namespace Kernel
