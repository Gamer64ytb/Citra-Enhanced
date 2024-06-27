// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <boost/serialization/base_object.hpp>
#include <boost/serialization/string.hpp>
#include <boost/serialization/vector.hpp>
#include "common/archives.h"
#include "common/logging/log.h"
#include "core/hle/kernel/errors.h"
#include "core/hle/kernel/k_address_arbiter.h"
#include "core/hle/kernel/k_auto_object_container.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_resource_limit.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/memory.h"

namespace Kernel {

class KAddressArbiter::Callback : public WakeupCallback {
public:
    explicit Callback(KAddressArbiter* _parent) : parent(_parent) {}
    KAddressArbiter* parent;

    void WakeUp(ThreadWakeupReason reason, KThread* thread,
                KSynchronizationObject* object) override {
        parent->WakeUp(reason, thread, object);
    }

private:
    template <class Archive>
    void serialize(Archive& ar, const unsigned int) {
        ar& boost::serialization::base_object<WakeupCallback>(*this);
    }
    friend class boost::serialization::access;
};

KAddressArbiter::KAddressArbiter(KernelSystem& kernel)
    : KAutoObjectWithSlabHeapAndContainer{kernel},
      m_timeout_callback(std::make_shared<Callback>(this)) {}

KAddressArbiter::~KAddressArbiter() = default;

void KAddressArbiter::Initialize(Process* owner) {
    m_owner = owner;
    m_owner->Open();
}

void KAddressArbiter::PostDestroy(uintptr_t arg) {
    Process* owner = reinterpret_cast<Process*>(arg);
    if (owner != nullptr) {
        owner->ReleaseResource(ResourceLimitType::AddressArbiter, 1);
        owner->Close();
    }
}

void KAddressArbiter::WaitThread(KThread* thread, VAddr wait_address) {
    thread->m_wait_address = wait_address;
    thread->m_status = ThreadStatus::WaitArb;
    m_waiting_threads.emplace_back(thread);
}

u64 KAddressArbiter::ResumeAllThreads(VAddr address) {
    // Determine which threads are waiting on this address, those should be woken up.
    auto itr = std::stable_partition(m_waiting_threads.begin(), m_waiting_threads.end(),
                                     [address](KThread* thread) {
                                         ASSERT_MSG(thread->GetStatus() == ThreadStatus::WaitArb,
                                                    "Inconsistent AddressArbiter state");
                                         return thread->m_wait_address != address;
                                     });

    // Wake up all the found threads
    const u64 num_threads = std::distance(itr, m_waiting_threads.end());
    std::for_each(itr, m_waiting_threads.end(), [](KThread* thread) { thread->ResumeFromWait(); });

    // Remove the woken up threads from the wait list.
    m_waiting_threads.erase(itr, m_waiting_threads.end());
    return num_threads;
}

bool KAddressArbiter::ResumeHighestPriorityThread(VAddr address) {
    // Determine which threads are waiting on this address, those should be considered for wakeup.
    auto matches_start = std::stable_partition(
        m_waiting_threads.begin(), m_waiting_threads.end(), [address](KThread* thread) {
            ASSERT_MSG(thread->GetStatus() == ThreadStatus::WaitArb,
                       "Inconsistent AddressArbiter state");
            return thread->m_wait_address != address;
        });

    // Iterate through threads, find highest priority thread that is waiting to be arbitrated.
    // Note: The real kernel will pick the first thread in the list if more than one have the
    // same highest priority value. Lower priority values mean higher priority.
    auto itr =
        std::min_element(matches_start, m_waiting_threads.end(), [](KThread* lhs, KThread* rhs) {
            return lhs->GetCurrentPriority() < rhs->GetCurrentPriority();
        });

    if (itr == m_waiting_threads.end()) {
        return false;
    }

    auto thread = *itr;
    thread->ResumeFromWait();
    m_waiting_threads.erase(itr);

    return true;
}

void KAddressArbiter::WakeUp(ThreadWakeupReason reason, KThread* thread,
                             KSynchronizationObject* object) {
    ASSERT(reason == ThreadWakeupReason::Timeout);
    // Remove the newly-awakened thread from the Arbiter's waiting list.
    m_waiting_threads.erase(std::remove(m_waiting_threads.begin(), m_waiting_threads.end(), thread),
                            m_waiting_threads.end());
};

Result KAddressArbiter::ArbitrateAddress(KThread* thread, ArbitrationType type, VAddr address,
                                         s32 value, u64 nanoseconds) {
    switch (type) {

    // Signal thread(s) waiting for arbitrate address...
    case ArbitrationType::Signal: {
        u64 num_threads{};

        // Negative value means resume all threads
        if (value < 0) {
            num_threads = ResumeAllThreads(address);
        } else {
            // Resume first N threads
            for (s32 i = 0; i < value; i++) {
                num_threads += ResumeHighestPriorityThread(address);
            }
        }

        // Prevents lag from low priority threads that spam svcArbitrateAddress and wake no threads
        // The tick count is taken directly from official HOS kernel. The priority value is one less
        // than official kernel as the affected FMV threads dont meet the priority threshold of 50.
        // TODO: Revisit this when scheduler is rewritten and adjust if there isn't a problem there.
        auto* core = m_kernel.current_cpu;
        if (num_threads == 0 && core->GetID() == 0 && thread->GetCurrentPriority() >= 49) {
            core->GetTimer().AddTicks(1614u);
        }
        break;
    }
    // Wait current thread (acquire the arbiter)...
    case ArbitrationType::WaitIfLessThan:
        if ((s32)m_kernel.memory.Read32(address) < value) {
            WaitThread(thread, address);
        }
        break;
    case ArbitrationType::WaitIfLessThanWithTimeout:
        if ((s32)m_kernel.memory.Read32(address) < value) {
            thread->SetWakeupCallback(m_timeout_callback);
            thread->WakeAfterDelay(nanoseconds);
            WaitThread(thread, address);
        }
        break;
    case ArbitrationType::DecrementAndWaitIfLessThan: {
        s32 memory_value = m_kernel.memory.Read32(address);
        if (memory_value < value) {
            // Only change the memory value if the thread should wait
            m_kernel.memory.Write32(address, (s32)memory_value - 1);
            WaitThread(thread, address);
        }
        break;
    }
    case ArbitrationType::DecrementAndWaitIfLessThanWithTimeout: {
        s32 memory_value = m_kernel.memory.Read32(address);
        if (memory_value < value) {
            // Only change the memory value if the thread should wait
            m_kernel.memory.Write32(address, (s32)memory_value - 1);
            thread->SetWakeupCallback(m_timeout_callback);
            thread->WakeAfterDelay(nanoseconds);
            WaitThread(thread, address);
        }
        break;
    }

    default:
        LOG_ERROR(Kernel, "unknown type={}", type);
        return ResultInvalidEnumValueFnd;
    }

    // The calls that use a timeout seem to always return a Timeout error even if they did not put
    // the thread to sleep
    if (type == ArbitrationType::WaitIfLessThanWithTimeout ||
        type == ArbitrationType::DecrementAndWaitIfLessThanWithTimeout) {
        return ResultTimeout;
    }

    return ResultSuccess;
}

template <class Archive>
void KAddressArbiter::serialize(Archive& ar, const unsigned int file_version) {
    ar& boost::serialization::base_object<KAutoObject>(*this);
    ar& m_name;
    ar& m_waiting_threads;
    // ar& m_timeout_callback;
}

SERIALIZE_IMPL(KAddressArbiter)

} // namespace Kernel

namespace boost::serialization {

template <class Archive>
void save_construct_data(Archive& ar, const Kernel::KAddressArbiter::Callback* t,
                         const unsigned int) {
    ar << t->parent;
}

template <class Archive>
void load_construct_data(Archive& ar, Kernel::KAddressArbiter::Callback* t, const unsigned int) {
    Kernel::KAddressArbiter* parent;
    ar >> parent;
    ::new (t) Kernel::KAddressArbiter::Callback(parent);
}

} // namespace boost::serialization

SERIALIZE_EXPORT_IMPL(Kernel::KAddressArbiter)
SERIALIZE_EXPORT_IMPL(Kernel::KAddressArbiter::Callback)
