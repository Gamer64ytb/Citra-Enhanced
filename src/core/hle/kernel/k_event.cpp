// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <boost/serialization/base_object.hpp>
#include "common/archives.h"
#include "common/assert.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_resource_limit.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"

SERIALIZE_EXPORT_IMPL(Kernel::KEvent)

namespace Kernel {

KEvent::KEvent(KernelSystem& kernel) : KAutoObjectWithSlabHeapAndContainer(kernel) {}

KEvent::~KEvent() = default;

void KEvent::Initialize(Process* owner, ResetType reset_type) {
    // Open a reference to the owner process.
    if (owner) {
        owner->Open();
        m_owner = owner;
    }

    // Set member variables.
    m_reset_type = reset_type;
}

void KEvent::PostDestroy(uintptr_t arg) {
    Process* owner = reinterpret_cast<Process*>(arg);
    if (owner != nullptr) {
        owner->ReleaseResource(ResourceLimitType::Event, 1);
        owner->Close();
    }
}

bool KEvent::ShouldWait(const KThread* thread) const {
    return !m_signaled;
}

void KEvent::Acquire(KThread* thread) {
    ASSERT_MSG(!ShouldWait(thread), "object unavailable!");
    if (m_reset_type == ResetType::OneShot) {
        m_signaled = false;
    }
}

void KEvent::Signal() {
    m_signaled = true;
    this->WakeupAllWaitingThreads();
}

void KEvent::Clear() {
    m_signaled = false;
}

void KEvent::WakeupAllWaitingThreads() {
    KSynchronizationObject::WakeupAllWaitingThreads();
    if (m_reset_type == ResetType::Pulse) {
        m_signaled = false;
    }
}

template <class Archive>
void KEvent::serialize(Archive& ar, const u32 file_version) {
    ar& boost::serialization::base_object<KSynchronizationObject>(*this);
    ar& m_owner;
    ar& m_reset_type;
    ar& m_signaled;
}

SERIALIZE_IMPL(KEvent)

} // namespace Kernel
