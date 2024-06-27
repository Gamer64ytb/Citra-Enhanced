// Copyright 2023 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <boost/serialization/base_object.hpp>
#include <boost/serialization/export.hpp>
#include <boost/serialization/string.hpp>
#include "common/archives.h"
#include "core/hle/kernel/errors.h"
#include "core/hle/kernel/k_auto_object_container.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_resource_limit.h"
#include "core/hle/kernel/k_semaphore.h"
#include "core/hle/kernel/kernel.h"

SERIALIZE_EXPORT_IMPL(Kernel::KSemaphore)

namespace Kernel {

KSemaphore::KSemaphore(KernelSystem& kernel) : KAutoObjectWithSlabHeapAndContainer(kernel) {}

KSemaphore::~KSemaphore() = default;

void KSemaphore::Initialize(Process* owner, s32 initial_count, s32 max_count, std::string name) {
    // Open a reference to the owner process.
    if (owner) {
        owner->Open();
        m_owner = owner;
    }

    // Set member variables
    m_available_count = initial_count;
    m_max_count = max_count;
    m_name = name;
}

void KSemaphore::PostDestroy(uintptr_t arg) {
    Process* owner = reinterpret_cast<Process*>(arg);
    if (owner != nullptr) {
        owner->ReleaseResource(ResourceLimitType::Semaphore, 1);
        owner->Close();
    }
}

bool KSemaphore::ShouldWait(const KThread* thread) const {
    return m_available_count <= 0;
}

void KSemaphore::Acquire(KThread* thread) {
    if (m_available_count <= 0) {
        return;
    }
    --m_available_count;
}

Result KSemaphore::Release(s32* out_count, s32 release_count) {
    R_UNLESS(release_count + m_available_count <= m_max_count, ResultOutOfRangeKernel);

    // Update available count.
    const s32 previous_count = m_available_count;
    m_available_count += release_count;

    // Wakeup waiting threads and return.
    this->WakeupAllWaitingThreads();
    *out_count = previous_count;
    return ResultSuccess;
}

template <class Archive>
void KSemaphore::serialize(Archive& ar, const u32 file_version) {
    ar& boost::serialization::base_object<KSynchronizationObject>(*this);
    ar& m_max_count;
    ar& m_available_count;
}

SERIALIZE_IMPL(KSemaphore)

} // namespace Kernel
